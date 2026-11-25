# MO — The Mount Olympus Spec Runner

> **MO = Mount Olympus = Modus Operandi.** It evaluates an agent's *method of
> operation* against what was declared. Zeus watches from above; MO is the part
> that judges.

MO consumes an observation stream, evaluates a Zeus behavioral spec against it,
tracks open obligations, and emits verdicts. It is not the proxy and not the VM.
It is the judge.

---

## 0. Decisions locked (don't relitigate without a reason)

- **Vantage point:** protocol-layer proxy. No ptrace/eBPF. Wire-level only.
- **Matching model:** *satisfaction within a window*, NOT strict ordering.
- **Core data structure:** a concurrent **obligation ledger**, not a linear cursor.
- **Protocol coupling:** translation lives in the **adapter**, outside MO. MO
  speaks only Zeus primitives and stays protocol-agnostic.
- **Verdict vocabulary:** `FULFILLED`, `BOLTED` (liveness), `CONDEMNED` (safety).

---

## 1. The architectural consequence of "satisfaction in window"

The old sketch had a `spec_cursor` walking the script line by line. Relaxing to
satisfaction-in-window **dissolves the cursor**. Replace it with:

- The spec is parsed once into a set of **rules**.
- Some rules are *unconditional* obligations (must happen this session).
- Most are *triggered* obligations: `OBSERVE X → EXPECT Y within W`.
- When a trigger fires, MO **opens an obligation** keyed by identifier.
- Incoming events **close** obligations (FULFILLED) regardless of intervening
  traffic.
- Expired obligations **self-close** as BOLTED.

No global ordering is enforced. Each obligation carries its own clock. This is
the trace-containment problem relaxed to *eventual satisfaction*, which is
exactly what survives contact with async, multi-connection agents.

---

## 2. Pipeline

```
MCP server (the god)
   ↓ stdio / SSE
[ Adapter / Proxy ]      protocol-specific; emits Zeus primitives + timestamps
   ↓ ZeusEvent stream
[ MO ]                   protocol-agnostic judge
   ├─ Spec Loader        .zspec → rule set
   ├─ Obligation Ledger  open / fulfilled / bolted
   ├─ Verdict Engine     applies rules to each event
   └─ Reporter           emits structured verdict log
   ↓
Audit report (JSONL + human summary)
```

The adapter↔MO boundary is the seam that lets you point MO at A2A or any other
protocol later without touching the judge.

---

## 3. Spec language (.zspec) — minimal v0

Three authoring primitives. `BOLTED`/`CONDEMNED`/`FULFILLED` are *emitted by MO*,
never written by hand.

```
OBSERVE  <event_pattern> [as <id>]      ; note that something crossed the wire
EXPECT   <event_pattern> [for <id>]     ; open a liveness obligation
WINDOW   <ms>                           ; clock on the most recent EXPECT
ASSERT   <predicate>                    ; safety check; failure → CONDEMNED
```

Worked example — every declared tool must actually be called within 5s:

```
on  tool_declared as $tool:
    EXPECT tool_called for $tool
    WINDOW 5000

ASSERT no tool_called where $tool not in declared_tools   ; → CONDEMNED
```

The `$tool` binding is what ties a declaration to its expected execution across
time. That identifier is the crime scene on the BOLT.

---

## 4. Data shapes

```python
@dataclass
class ZeusEvent:                 # what the adapter emits
    primitive: str               # "tool_declared", "tool_called", "send", ...
    identifier: str | None       # binding key, e.g. tool name
    payload: dict                # raw, for ASSERT predicates
    ts: float                    # monotonic ms

@dataclass
class Obligation:                # a row in the ledger
    identifier: str
    expects: str                 # primitive that satisfies it
    opened_at: float
    expires_at: float            # opened_at + window
    spec_line: int
    state: str                   # "open" | "fulfilled" | "bolted"

@dataclass
class Verdict:
    kind: str                    # FULFILLED | BOLTED | CONDEMNED
    identifier: str | None
    spec_line: int
    opened_at: float | None
    closed_at: float
    detail: dict
```

A BOLT entry, concretely:

```json
{ "kind":"BOLTED", "identifier":"tool_x", "spec_line":14,
  "opened_at":0.0, "closed_at":5001.0,
  "detail":{ "window_ms":5000, "reason":"liveness_window_expired",
              "lapsed_at":5000.0, "notice_lag_ms":1.0 } }
```

---

## 5. The evaluation loop (revised — no cursor)

```python
def run(stream, spec):
    ledger, verdicts = ObligationLedger(), []

    for ev in stream:
        # a) expire first, against THIS event's timestamp (deterministic)
        for ob in ledger.open_obligations():
            if ev.ts > ob.expires_at:
                verdicts.append(bolt(ob)); ledger.close(ob, "bolted")

        # b) satisfy any open obligation this event closes
        for ob in ledger.open_obligations():
            if ev.primitive == ob.expects and ev.identifier == ob.identifier:
                verdicts.append(fulfill(ob)); ledger.close(ob, "fulfilled")

        # c) safety: does this event violate an ASSERT?
        for rule in spec.assertions:
            if rule.violated_by(ev):
                verdicts.append(condemn(ev, rule))

        # d) does this event TRIGGER new obligations?
        for rule in spec.triggers:
            if rule.fires_on(ev):
                ledger.open(rule.make_obligation(ev))

    # e) end of stream: every still-open obligation is a BOLT
    for ob in ledger.open_obligations():
        verdicts.append(bolt(ob, reason="session_ended_unresolved"))

    return verdicts
```

**Why expire-against-event-ts, not wall clock:** makes replay deterministic. The
same JSONL trace always produces the same verdicts. Critical for testing and for
trusting the audit. (See premortem.)

**End-of-stream BOLT** is its own important class: the agent disconnected with
oaths still open.

---

## 6. Milestones (sequenced, dogfood at each step)

**M0 — Replay-only core. ✅ DONE.** MO consumes a *recorded* JSONL ZeusEvent file
(not a live proxy). Implements the loop above. Hand-built rule objects + tiny event
logs prove FULFILLED + BOLTED + CONDEMNED all fire. No networking.
*Exit met:* `python3 -m mo eval <spec.py> <trace.jsonl>` prints a verdict log.
Lives in `mo/` (Python sibling package; never imports the C VM). 14 tests pass
(`python3 -m pytest mo/`). §9 demo verified: clean→clean, unhonored tool→one BOLT.

Decisions locked while building M0:
- **Spec form in M0 is a `.py` file** exposing a module-level `spec` (rules.Spec).
  `.zspec` text grammar is M1; `cli.load_spec` is the seam it slots behind.
- **Output split:** verdicts as JSONL on **stdout**, human summary on **stderr** —
  so `mo eval ... | jq` stays clean. (Resolves §8: yes, log FULFILLED — coverage.)
- **Exit code:** 0 clean, 1 if any BOLTED/CONDEMNED → usable as a CI gate.
- **Window boundary is inclusive:** expiry uses strict `ev.ts > expires_at`, so an
  event landing exactly at `opened_at + window` still FULFILLS.
- **Coverage = unmatched count** (premortem #5). An event counts as *matched* only
  if it fulfills / condemns / triggers; pure expiry-carrier traffic stays unmatched.
- **BOLT `closed_at` = notice-time** (the event ts that revealed the lapse).
  ✅ Resolved at M4: a lapsed BOLT also records `detail.lapsed_at` (the exact
  window boundary) and `detail.notice_lag_ms`, so a late flush or coarse tick
  cannot make a 5s window look like a longer obligation. `closed_at` stays
  notice-time so the verdict stream remains monotonic.
  Demo: `python3 -m mo.examples.lapse_time_demo --out lapse.html`.

**M1 — Spec parser. ✅ DONE.** `mo/parser.py` turns `.zspec` text into the same
TriggerRule/AssertRule objects — engine/ledger/report untouched. Line-oriented
with assembler-style `line N:` errors. `cli.load_spec` dispatches on extension
(`.zspec` → parse, `.py` → import, back-compat). 22 tests pass.
*Exit met:* `mo eval mo/examples/every_tool_honored.zspec <trace>` runs the §9
demo from a text spec; `spec_line` in verdicts points at real source lines.

Grammar v0 (locked, deliberately small per premortem #3):
```
; comment to end of line
INCLUDE <path>
on <primitive> as $<bind>:
    EXPECT <primitive> for $<bind>
    WINDOW <ms>

ASSERT no <primitiveA> where identifier not in <primitiveB> [GRACE <ms>]
```
- `on ... as $x:` IS the OBSERVE primitive (block form from §3's worked example).
  One EXPECT + one WINDOW per block; binding `$x` must match across `on`/`EXPECT`.
- ASSERT v0 is **membership-only**. The "set" is a primitive's seen-identifiers
  (what `engine.seen` already tracks) — no named-set indirection like
  `declared_tools`. Equality/boolean forms deferred until something real needs them.
- `GRACE <ms>` is the one temporal addition: a violating call opens a pending
  check; a declaration of the same identifier at or before the deadline resolves
  it, while a tick, later event, or end-of-stream past the deadline condemns it.
  It exists for wire-order live mode, not as a general temporal query language.
- `INCLUDE <path>` composes another `.zspec` at that point, resolved relative
  to the including file. It is include-once (a diamond adds one copy) and a
  cycle or missing file is a line-numbered parse error. Rules keep the file
  they came from as `spec_source`, so a line number stays unambiguous across
  fragments.

**M2 — Adapter for MCP.** Write the protocol adapter that turns the existing tap
session logs (`from_mcp_session()` already exists!) into a ZeusEvent stream. This
is where MO meets your existing Glassport work.
*Exit:* feed a real captured MCP session through MO, get verdicts.

**M3 — Live mode. ✅ DONE.** MO tails a *growing* glassport session log and
judges it in real time, minting a synthetic `tick` so liveness windows expire
during silence (premortem #1). `mo watch [--idle-ms N] <spec> <session.jsonl>`
streams verdicts to stdout the instant each fires; the deferred launcher form
`mo watch --log-dir DIR <spec> -- <command...>` starts the tap itself and tails
the session log it creates. 40 tests pass at M3 (112 in the suite today).
*Exit met:* a hung server (calls a tool, never answers) yields exactly one BOLT
with reason `liveness_window_expired` — proven on both a static log and a file
appended-to live while MO tails it.

Decisions locked while building M3:
- **`tick` is a first-class Zeus primitive,** recognized by the engine
  (`engine.py`): it advances the clock and expires windows but never fulfills,
  condemns, triggers, or counts toward coverage (premortem #5). "Time passed" is
  protocol-agnostic, so this doesn't breach the adapter seam (#4).
- **The wall clock lives in the adapter, never the engine.** `adapters/live.py`
  reads `time.time()` to mint ticks; the engine still only reads `ev.ts`, so
  replay determinism (#6) is preserved by construction. clock/sleep/stop are
  injectable so the tick logic is deterministically testable without real time.
- **Tick ts is on the same scale as frame ts (epoch ms)** so `ev.ts >
  expires_at` is meaningful. The tick keeps the historical **100ms** ceiling
  but tightens to one tenth of the tightest declared window (5ms floor, poll
  coupled) — the §8 measurement decision; `mo watch --tick-ms` overrides it.
- **One evaluation loop, two faces:** `iter_verdicts()` is the generator;
  `run()` drains it into a RunResult for replay, `watch()` consumes it lazily so
  a BOLT prints mid-session, not at end-of-stream.
- **Live projection is WIRE ORDER** (`frame_projector`), not declarations-up-
  front like the batch `--mcp` path: live can't pre-scan the session. This means
  a membership ASSERT is racy against a late `tools/list` (premortem #2's
  cousin). The `GRACE <ms>` variant handles it without pre-scanning: a call
  opens a deferred check, a declaration inside the window resolves it, and a
  tick/later event/end-of-stream past the deadline condemns it. The
  pre-delivery gate escalates a pending check rather than allowing an effect it
  cannot yet clear.
- **Departed from the literal `-- <mcp server cmd>` exit line:** glassport's tap
  writes a JSONL file (no push API) and needs a real client to drive the server,
  so "read the proxy stream in real time" became *tail the growing tap log* —
  the testable, dogfoodable seam. The deferred `-- <cmd>` launcher now wraps it:
  with `--log-dir DIR`, MO snapshots the directory, starts the command in its
  own process group, waits for the new `*.jsonl`, and stops the child group when
  judging ends (child failure exit 1, missing log exit 2). Demo:
  `python3 -m mo.examples.watch_launcher_demo`.

**M4 — Report. ✅ DONE.** `report.render_html(result)` draws a self-contained,
no-JS, offline HTML report: FULFILLED green spans, BOLTED amber spans,
CONDEMNED red point-markers, in even verdict lanes, under a CLEAN/DEVIATIONS
banner with the coverage signal. `mo report [--mcp] <spec> <trace> [-o out.html]`.
44 tests pass.
*Exit met:* one artifact you can show someone — generated for the §9 fabrication
session (2 green + 1 red) and the hung-server session (1 amber BOLT).

Decisions locked while building M4:
- **Every identifier is HTML-escaped** before it touches the page (matches the
  Glassport report's discipline). A hostile server can name a tool
  `<img onerror=...>` and the report opens from `file://` where injected script
  runs with local reach — so wire strings render as text, never markup. A test
  pins this with a live XSS-shaped identifier.
- **Even-lane geometry** (`_span_geometry`): each verdict occupies one lane in
  verdict order, 72% of the lane width. This replaced the linear time scale
  after a dense session collapsed millisecond calls beside a six-second BOLT
  into a sliver. True durations moved into the bar tooltips; the axis labels
  lanes, not milliseconds, and the session range stays in the subtitle. Pinned
  by `test_dense_verdicts_use_even_lanes_and_tooltip_durations`.
- **CONDEMNED is a point marker, not a span** — it has no `opened_at`, so it
  renders at its instant (width 0, red left-border) rather than a bar.
- **Zero dependencies, single string** — no template engine, inline CSS, opens
  on a phone. Consistent with the Glassport family and MO's no-C-import rule.

**M5 — Pre-delivery gate. ✅ DONE.** `engine.Judge.observe()` evaluates one
event without finalising: no end-of-stream BOLT, no wall clock. `finish()` is
the only finaliser. `mo/gate.py` wraps that in a `Gate` returning `ALLOW`,
`DENY` or `ESCALATE` *before* an effect is delivered. `ALLOW` requires a rule to
claim the event; an unmatched effect **escalates** rather than passing as "fine"
(premortem #5), and an ASSERT violation denies. The gate judges on a forked
ledger and commits only on `ALLOW`, so a refused effect never enters MO's state.
65 passed, 2 skipped.

---

## 7. PREMORTEM — how MO is wrong in 6 months

1. **The silence problem (highest risk).** In replay, time advances only when
   events arrive. If an agent declares a tool then goes *quiet*, no event ever
   carries the timestamp that would expire the window — the BOLT never fires in
   live mode. **Mitigation:** M3's synthetic tick. Decide the tick granularity
   now; too coarse misses tight windows, too fine burns CPU. Start at 100ms.

2. **Identifier collisions.** Two concurrent calls to the same tool open two
   obligations with the same `identifier`. Which `tool_called` closes which?
   Right now: first-match. **Risk:** a late fulfillment closes the wrong (newer)
   obligation and an older one BOLTs spuriously. **Mitigation:** obligations may
   need an instance/correlation id, not just the tool name. Watch for it at M2
   with real traffic.
   **✅ Mitigated:** both MCP adapters attach the jsonrpc id as `correlation`;
   an obligation opened by a correlated event only closes on an event with the
   same id. Matching is scoped to the obligation's own id: a declaration has no
   instance id, so its obligation accepts the first matching execution rather
   than vetoing every correlated call. Pinned by
   `test_concurrent_same_tool_results_close_matching_obligation`,
   `test_uncorrelated_declaration_obligation_accepts_correlated_call` and
   `test_declared_tool_honored_by_correlated_call_is_fulfilled`.

3. **ASSERT scope creep.** `ASSERT` predicates are a tiny language that wants to
   grow into a full query DSL. **Mitigation:** cap v0 predicates to
   membership + equality. Resist `where x AND (y OR z)` until something real
   demands it.

4. **Adapter leaks protocol into MO.** The temptation under deadline is to let
   MO peek at MCP-shaped payloads "just this once." That collapses the seam and
   kills the A2A story. **Mitigation:** MO test suite uses *only* synthetic
   ZeusEvents that never mention MCP. If MO can't be tested without the adapter,
   the seam has leaked.

5. **No spec = no findings, looks like success.** An empty or mis-targeted spec
   produces a clean report. Silence reads as a pass. **Mitigation:** MO reports
   *coverage* — how many events matched no rule at all. A high unmatched count is
   itself a signal the spec is blind.

6. **Determinism rot.** The moment any rule consults `wall_clock()` instead of
   `ev.ts`, replay stops reproducing. **Mitigation:** ban wall-clock reads inside
   the engine; inject time only via events/ticks. Make it a lint rule.
   **✅ Mitigated:** `mo/determinism.py` AST-scans the six judge-core modules and
   reports every `time`/`datetime`/`timeit` import or call, aliases included.
   `mo/tests/test_determinism.py` fails the suite on a finding; the CLI and
   adapters stay outside because that is where live ticks are minted. The proof
   page is `python3 -m mo.examples.determinism_guard_demo --out proof.html`.

---

## 8. Open questions (decide before M2)

- ~~Does an obligation need a correlation id beyond the human identifier?~~
  **DECIDED (M2/M4): yes** — adapters attach the jsonrpc id as `correlation`,
  and only the obligation's own id constrains matching. A declaration's
  obligation (no instance id) accepts the first matching execution.
- ~~Tick granularity for live mode.~~ **DECIDED (measured):** 100ms stays the
  ceiling for ordinary multi-second windows; `watch` tightens the tick to one
  tenth of the tightest declared window (5ms floor) and shrinks the poll to
  half the tick (50ms cap), so the worst-case BOLT notice lag is about 10% of
  the window rather than one fixed tick. Measured on the real tailer with a
  virtual clock at 20–400 polls/s by
  `python3 -m mo.examples.tick_granularity_demo`. An idle return also emits
  one final tick, so a window that lapsed during the last quiet interval is
  reported as `liveness_window_expired` with an exact `lapsed_at`, not as
  `session_ended_unresolved`.
- ~~Should `FULFILLED` even be logged?~~ **DECIDED (M0): yes** — gives coverage
  stats for free; engine logs all three kinds.
- ~~One spec per server, or composable spec fragments?~~ **DECIDED (implemented):**
  composable fragments. A root spec composes shared files with
  `INCLUDE <path>`, resolved relative to the including file. Inclusion is
  include-once, so a diamond (two fragments sharing a base) adds one copy of
  the shared rules rather than two obligations per event; a cycle or missing
  file is a line-numbered parse error. Every rule, obligation and verdict
  carries `spec_source`, so a verdict's line number stays unambiguous across
  files, while single-file specs keep their old JSONL shape and `L<n>` label.
  Demo: `python3 -m mo.examples.spec_fragments_demo`.

Decided in M0, resolved at M4:
- **Identifier collisions (premortem #2)** are modeled by `correlation`.
  An obligation opened by a correlated event demands the same id on its
  closer; a declaration's obligation is anonymous and accepts the first
  matching execution. Uncorrelated events still fall back to identifier
  matching for M0/M1 replay compatibility.

---

## 9. The one-line test of whether this is real

> Run a correct MCP server through MO → clean report.
> Introduce a server that declares a tool and never honors it → exactly one BOLT,
> on the right identifier, at the right time.

If that demo works, the whole Olympus thesis is real and you build outward. If it
doesn't, you learn why before betting months on it.

**✅ Verified:** `python3 -m mo.examples.real_failure_demo` starts a real
`glassport probe` against `mo/examples/unhonored_mcp_server.py` (declares
`report_export`, never answers it), tails the tap with `mo watch`, and asserts
exactly one `BOLTED` verdict, `liveness_window_expired`, while glassport is
still running. The live report is written with `--out`; the demo is
self-checking and exits non-zero on a missed or end-of-stream BOLT.
