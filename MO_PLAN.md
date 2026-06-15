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
  "detail":{ "window_ms":5000, "reason":"liveness_window_expired" } }
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
- **BOLT `closed_at` = notice-time** (the event ts that revealed the lapse), not the
  exact lapse instant. Revisit at M4 if reports need lapse-time.

**M1 — Spec parser. ✅ DONE.** `mo/parser.py` turns `.zspec` text into the same
TriggerRule/AssertRule objects — engine/ledger/report untouched. Line-oriented
with assembler-style `line N:` errors. `cli.load_spec` dispatches on extension
(`.zspec` → parse, `.py` → import, back-compat). 22 tests pass.
*Exit met:* `mo eval mo/examples/every_tool_honored.zspec <trace>` runs the §9
demo from a text spec; `spec_line` in verdicts points at real source lines.

Grammar v0 (locked, deliberately small per premortem #3):
```
; comment to end of line
on <primitive> as $<bind>:
    EXPECT <primitive> for $<bind>
    WINDOW <ms>

ASSERT no <primitiveA> where identifier not in <primitiveB>
```
- `on ... as $x:` IS the OBSERVE primitive (block form from §3's worked example).
  One EXPECT + one WINDOW per block; binding `$x` must match across `on`/`EXPECT`.
- ASSERT v0 is **membership-only**. The "set" is a primitive's seen-identifiers
  (what `engine.seen` already tracks) — no named-set indirection like
  `declared_tools`. Equality/boolean forms deferred until something real needs them.

**M2 — Adapter for MCP.** Write the protocol adapter that turns the existing tap
session logs (`from_mcp_session()` already exists!) into a ZeusEvent stream. This
is where MO meets your existing Glassport work.
*Exit:* feed a real captured MCP session through MO, get verdicts.

**M3 — Live mode. ✅ DONE.** MO tails a *growing* glassport session log and
judges it in real time, minting a synthetic `tick` so liveness windows expire
during silence (premortem #1). `mo watch [--idle-ms N] <spec> <session.jsonl>`
streams verdicts to stdout the instant each fires. 40 tests pass.
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
  expires_at` is meaningful. Default tick granularity **100ms** (premortem #1).
- **One evaluation loop, two faces:** `iter_verdicts()` is the generator;
  `run()` drains it into a RunResult for replay, `watch()` consumes it lazily so
  a BOLT prints mid-session, not at end-of-stream.
- **Live projection is WIRE ORDER** (`frame_projector`), not declarations-up-
  front like the batch `--mcp` path: live can't pre-scan the session. This means
  the membership ASSERT is racy against a late `tools/list` (premortem #2's
  cousin), so the M3 demo spec is liveness-only. Wire-order fabrication checks
  wait for a correlation/declaration-aware ASSERT (M4+).
- **Departed from the literal `-- <mcp server cmd>` exit line:** glassport's tap
  writes a JSONL file (no push API) and needs a real client to drive the server,
  so "read the proxy stream in real time" became *tail the growing tap log* —
  the testable, dogfoodable seam. A `-- <cmd>` launcher can wrap it later.

**M4 — Report. ✅ DONE.** `report.render_html(result)` draws a self-contained,
no-JS, offline HTML timeline: FULFILLED green spans, BOLTED amber spans,
CONDEMNED red point-markers, over a linear-time axis, under a CLEAN/DEVIATIONS
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
- **Linear time scale** (`_span_geometry`): x is proportional to real elapsed
  ms, so a BOLT's window genuinely looks wide and clustered calls sit close.
  **KNOWN LIMITATION:** on a dense session — many calls milliseconds apart next
  to one multi-second BOLT — the fast calls collapse to a sliver and the report
  reads badly. The fix is per-event (even-lane) spacing with true durations in
  tooltips, ~6 lines isolated in `_span_geometry`. Left linear until a real
  dense session proves it unreadable; swap is local and test-pinned.
- **CONDEMNED is a point marker, not a span** — it has no `opened_at`, so it
  renders at its instant (width 0, red left-border) rather than a bar.
- **Zero dependencies, single string** — no template engine, inline CSS, opens
  on a phone. Consistent with the Glassport family and MO's no-C-import rule.

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

---

## 8. Open questions (decide before M2)

- Does an obligation need a correlation id beyond the human identifier? (see
  premortem #2 — probably yes, defer until real traffic proves it)
- Tick granularity for live mode. (start 100ms, measure)
- ~~Should `FULFILLED` even be logged?~~ **DECIDED (M0): yes** — gives coverage
  stats for free; engine logs all three kinds.
- One spec per server, or composable spec fragments? (start single, design
  parser so fragments are possible later)

Decided in M0, still open at the margins:
- **Identifier collisions (premortem #2)** remain unmodeled — `run()` uses
  first-match by `(primitive, identifier)`. Two concurrent `tool_declared search`
  open two obligations; the next `tool_called search` closes whichever is still
  open by list order. Fine for M0; revisit with correlation ids at M2.

---

## 9. The one-line test of whether this is real

> Run a correct MCP server through MO → clean report.
> Introduce a server that declares a tool and never honors it → exactly one BOLT,
> on the right identifier, at the right time.

If that demo works, the whole Olympus thesis is real and you build outward. If it
doesn't, you learn why before betting months on it.
