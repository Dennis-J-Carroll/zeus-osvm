# CLAUDE.md — Zeus_osVM

Stack-based VM in C for experimenting with networking concepts. Builds `zeus` (VM + CLI), `zasm` (assembler wrapper), `zdis` (disassembler wrapper).

## Build & Test

- Build: `make` (produces `zeus`, `zasm`, `zdis`)
- Test: `make test`
- Smoke test: `make smoke` (builds + runs `./smoke_test.sh` end-to-end against all examples)
- Clean: `make clean` (also removes `examples/*.zeus` and the `zasm`/`zdis` wrappers)

## CLI (src/main.c)

The `zeus` binary dispatches on `argv[1]`:

```bash
zeus run prog.zeus            # run a bytecode file
zeus asm in.zasm out.zeus     # assemble .zasm -> .zeus
zeus dis prog.zeus            # disassemble bytecode
zeus run-asm in.zasm          # assemble + run in one step
```

`zasm` and `zdis` are generated shell wrappers (see Makefile) that `exec ./zeus asm` / `./zeus dis`. They are recreated by `make` and deleted by `make clean`.

## Docs

`docs/USAGE.md` — comprehensive usage + full opcode reference (verified against source). `README.md` for the human overview.

## Project Layout

- `include/` - public C headers
- `src/` - implementation
  - `zeus.c`, `isa.c`, `bytecode.c` - common utilities
  - `vm.c` - interpreter core
  - `net.c` - socket and raw-packet primitives
  - `asm.c` - assembler
  - `disasm.c` - disassembler
  - `main.c` - CLI entry point
- `examples/` - `.zasm` example programs
- `tests/` - C test harnesses
- `assets/` - logo and ASCII boot splash art
- `tools/` - helper scripts (logo-to-ASCII, boot-splash generator)

## Conventions

- C11, compile with `-Wall -Wextra`
- Little-endian bytecode
- 64-bit signed integer stack words
- Opcode bytes are defined in `include/isa.h`; metadata in `src/isa.c`
- VM memory layout: program data copied to address 0 at load time; rest is runtime heap/stack
- Separate operand and call stacks, flat runtime memory, BSD-sockets networking layer

## Adding New Instructions

1. Add opcode constant to `include/isa.h`
2. Add metadata to `src/isa.c`
3. Implement execution in `src/vm.c`
4. Add assembler support if it takes an operand (already handled generically by `has_operand`)
5. Add a test to `tests/test_vm.c`

## Raw Networking Notes

Raw packet opcodes create IP-layer raw sockets. On Linux this requires `CAP_NET_RAW` or root. The VM compiles without privileges but raw operations return `-1` at runtime if denied.

## Gotchas & Latent Capabilities

- **Bytecode header:** `.zeus` files start with `ZEUS_MAGIC` + `ZEUS_VERSION` (`src/bytecode.c`, `include/bytecode.h`). Loader rejects mismatched magic/version — bump `ZEUS_VERSION` when changing the format.
- **Assembler string limit:** single-line string literals are bounded by an 8192-byte buffer (`src/asm.c:257`). Long strings (e.g. boot splash) must stay under it.
- **Step mode exists but unexposed:** `zeus_vm_step` (`src/vm.c:138`, `include/vm.h`) drives single-instruction execution and the main run loop, but no debugger/step CLI is wired up. Reuse it if adding one.
- **Networking is single-threaded + blocking.** No select/poll loop yet.
- **Sockets:** both `SOCK_TCP` and `SOCK_UDP` (`0x71`) opcodes exist; README only highlights TCP.

## MO — spec runner (Python sibling)

`mo/` is a Python package that judges agent behavior against a `.zspec`; it
**never imports the C VM**. Verdicts: `FULFILLED`, `BOLTED` (liveness window
lapsed), `CONDEMNED` (safety assertion broke). Design in `MO_PLAN.md`.

```bash
python3 -m mo eval   spec.zspec trace.jsonl              # replay a recorded ZeusEvent stream
python3 -m mo eval   --mcp spec.zspec session.jsonl      # replay a captured Glassport MCP tap log
python3 -m mo watch  spec.zspec session.jsonl            # judge a growing tap log live (synthetic ticks expire silent windows)
python3 -m mo report spec.zspec trace.jsonl -o out.html  # self-contained HTML timeline
```

- Verdicts as JSONL on stdout, human summary on stderr; exit `1` if any
  BOLTED/CONDEMNED — usable as a CI gate. `--mcp`/`report --mcp` lazy-import the
  Glassport adapter so the protocol lib stays out of MO's core import graph.
- Tests: `python3 -m pytest mo/`. The seam is mechanical — `mo/tests/test_seam.py`
  asserts importing MO's core never loads `glassport` (keeps it protocol-agnostic).

## Verified Working

`make` builds all three binaries; `make test` passes 7/7. `make smoke` passes 8/8. `hello`, `echo_server`, `http_server`, `boot_splash` examples run; `raw_ping` needs `CAP_NET_RAW`/root. MO: `python3 -m pytest mo/` passes 60/60 (the live dogfood is marked `slow`; `-m "not slow"` runs 59). Golden adapter contract (`mo/tests/fixtures/`) and one-command live dogfood (`python3 -m mo.examples.live_dogfood`) both green.

Recent additions:
- Correlation ids: `ZeusEvent` and `Obligation` carry a `correlation` field so
  concurrent calls to the same tool close the right obligation (premortem #2).
- Live dogfood verified: `glassport wrap -- python3 fake_server.py` + `mo watch`
  on the growing tap log produced one `FULFILLED` verdict and no deviations.

## Note on AGENTS.md

`AGENTS.md` holds the same notes for other agent tools. Both files are maintained independently — **update both when changing build/layout/conventions** to avoid drift.

---

# Working on MO (Claude Code directives)

MO (`mo/`) is the live workstream. It's a protocol-agnostic judge that evaluates
an agent's behavior against a declared `.zspec`. Verdicts: `FULFILLED`,
`BOLTED` (liveness window lapsed), `CONDEMNED` (safety assertion broke). The full
design and the standing premortem are in `MO_PLAN.md` — **read it before changing
engine/ledger/rules/parser.** The premortem items are not suggestions; several
are enforced by tests.

## Run things

```bash
python3 -m pytest mo/                 # full suite (fast + the slow live test)
python3 -m pytest mo/ -m "not slow"   # skip the real-clock dogfood (CI inner loop)
python3 -m mo.examples.live_dogfood   # one-command live demo: 1 FULFILLED + 1 BOLT
python3 -m mo eval   --mcp spec.zspec session.jsonl   # judge a captured tap log
python3 -m mo watch  spec.zspec session.jsonl         # judge a growing log live
python3 -m mo report --mcp spec.zspec session.jsonl -o out.html
```

## Three laws — do not break these without saying so out loud

1. **The seam is sacred (premortem #4).** MO's core — `engine, ledger, rules,
   parser, trace, report, cli` — MUST NOT import `glassport` or any protocol lib.
   All MCP knowledge lives in `mo/adapters/`. `mo/tests/test_seam.py` enforces
   this mechanically; if it goes red, protocol knowledge has leaked and the
   A2A-portability story is dead. Never "just this once" peek at MCP shapes
   inside the engine.

2. **Determinism: the engine reads `ev.ts`, never a wall clock (premortem #6).**
   Time enters the engine ONLY through event/tick timestamps. The one allowed
   wall-clock read is in `mo/adapters/live.py` (minting ticks). If you find
   yourself wanting `time.time()` anywhere in the core, stop — inject it as a
   tick instead. Replay of the same JSONL must always produce the same verdicts.

3. **The adapter contract is frozen in `mo/tests/fixtures/` (premortem #5).**
   The golden fixtures pin the exact ZeusEvent stream MO must project from a
   real-shaped Glassport session. The core suite never imports glassport, so a
   drift in Glassport's `InteractionTrace` shape (a moved `jsonrpc_id`, a
   renamed `parts` field) would otherwise pass every test and silently break
   every real verdict. The golden is the trust boundary.

## When the golden test goes red

`test_adapter_golden.py` failing means the adapter's projection changed. That is
either a bug or a deliberate contract change — decide which:

- **Unintended** (you changed engine/adapter code and didn't mean to move the
  projection): the test caught a regression. Fix the code, don't touch the golden.
- **Intended** (Glassport's shape legitimately changed, or you're adding a field
  to ZeusEvent): regenerate, **review the diff**, and commit the new golden with
  a message explaining why the contract moved:

  ```bash
  python3 -m mo.tests.fixtures.regen
  git diff mo/tests/fixtures/      # READ THIS before committing
  ```

  Never regenerate reflexively to make a red test green. The diff is the point.

## Two paths, one known asymmetry — keep it intact

- `trace_to_events` (batch, `--mcp`): consumes a Glassport `InteractionTrace`,
  **hoists declarations to the front** so the fabrication ASSERT can see the
  declared set before judging any call. This path CAN catch a tool called but
  never declared.
- `frame_projector` (live, `watch`): wire-order, can't pre-scan, so a
  declaration arriving after a call is invisible to a membership ASSERT. This is
  the documented M3 racy-membership limitation, pinned by
  `test_batch_hoists_declarations_but_wire_order_does_not`. The live demo spec
  is therefore liveness-only. Don't "fix" the asymmetry by making live pre-scan
  — that breaks the streaming guarantee. The real fix (a grace-window ASSERT
  that tolerates late declarations) is a deliberate M4+ feature; see MO_PLAN §7
  and the premortem on ASSERT scope creep (#3) before building it.

## Correlation ids (premortem #2, closed)

`ZeusEvent` and `Obligation` carry a `correlation` (the JSON-RPC id). Matching
rule in `engine.iter_verdicts`: if either side has a correlation id they must
agree exactly; if neither does, fall back to first-match by identifier (the
M0/M1 behavior). This stops two concurrent calls to the same tool from
cross-closing. Preserve the fallback — specs without correlation data still work.

## ASSERT stays small (premortem #3)

ASSERT v0 is membership-only (`no X where identifier not in Y`). It wants to grow
into a query DSL. Resist. If a real need arrives, prefer modeling it as a new
ledger primitive (where time already lives) over adding logic — especially
temporal logic — inside ASSERT.

## House style

- Tests use `types.SimpleNamespace` to fake Glassport-shaped objects — never
  import glassport in a test outside the two `pytest.importorskip("glassport")`
  integration cases.
- New examples go in `mo/examples/`; new fixtures in `mo/tests/fixtures/` with a
  regen path. A fixture without a regen path is a liability.
- Keep MO zero-dependency and phone-runnable (Termux), consistent with the
  Glassport family. No template engines, no heavyweight deps.
