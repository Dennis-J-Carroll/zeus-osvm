# Zeus_osVM

![Zeus_osVM logo](assets/logo.jpg)

A fun, educational stack-based virtual machine written in C, designed for experimenting with networking concepts — with a suspendable machine, a default-deny effect policy and a hash-chained audit journal.

## Features

- Stack-based bytecode interpreter with 64-bit words
- Suspendable machines: `zeus save` / `zeus restore` snapshot the entire ~132 KiB machine, pending effect and all
- First-class socket opcodes: `SOCK_TCP`, `SOCK_UDP`, `BIND`, `LISTEN`, `ACCEPT`, `CONNECT`, `SEND`, `RECV`, `CLOSE`
- Raw packet opcodes: `RAW_ALLOC`, `RAW_FREE`, `RAW_SET_PROTO`, `RAW_SET_DST`, `RAW_SET_SRC`, `RAW_SET_PAYLOAD`, `RAW_SEND`, `RAW_RECV`
- Safety layer: default-deny effect policy, append-only hash-chained audit journal (`zeus journal verify` / `resolve`), `--ask` / `--approve` approval gate, and a Linux `zeus jail` (namespaces, uid drop, seccomp)
- `mo/`, a Python spec runner that judges agent behaviour against a `.zspec` and returns `FULFILLED`, `BOLTED` or `CONDEMNED`
- Small assembler (`zasm`) and disassembler (`zdis`)
- Example programs including an echo server, HTTP server, and raw ping

## Building

```bash
make
```

This produces `zeus`, `zasm`, and `zdis`.

## Documentation

Full usage guide — CLI, assembly language, complete opcode reference, worked
examples, and networking — in [`docs/USAGE.md`](docs/USAGE.md).

## Quick Start

Assemble and run the hello example:

```bash
./zasm examples/hello.zasm hello.zeus
./zeus run hello.zeus
```

Or assemble and run in one step:

```bash
./zeus run-asm examples/hello.zasm
```

Disassemble a bytecode file:

```bash
./zdis hello.zeus
```

## Suspend and resume

Every external operation travels the effect path: the interpreter yields a
request instead of performing it, so a stopped machine is just its state. Run
a fixed number of instructions, write the machine to disk, and pick it up
later — possibly more than once:

```bash
./zeus run-asm examples/hello.zasm --max-steps 3 --save m.zsnap
./zeus restore m.zsnap          # continues exactly where it stopped
```

Restoring the same snapshot twice gives two independent machines. Snapshots
are versioned and checksummed, written atomically (temporary + `fsync` +
`rename`), and a parked machine's file records the audit lineage and sequence
it belongs to, so a dossier cannot be paired with the wrong machine.

## Protection

On the effect path the interpreter performs no host I/O itself: sockets, raw
packets, `READ`, `PRINT` and `PRINTC` are yielded as effects and delivered by
the supervisor, which makes that seam a natural policy point. (`zeus_vm_step`
still delivers directly; the policy seam belongs to `zeus_vm_exec` /
`zeus_vm_resume`.)

- **Default-deny policy** — console and sockets are allowed; `RAW_*` effects
  need `--allow-raw`. A denied effect halts the machine (exit 4) instead of
  handing the program a failure result it can ignore. If the journal cannot
  accept the refusal, the effect is still cancelled but the run fails as an
  error: an unrecorded denial is never reported as a proven one.
- **Append-only audit** — `--journal F` writes `intent` before each effect and
  `commit` after, with a supervisor-owned sequence number and a digest of the
  resolved operands. The journal is held under an exclusive lock for the life
  of the supervisor, so two processes cannot scan the same counter and mint the
  same number. A crash between intent and commit is refused on every retry: a
  refused open writes nothing, so the unmatched intent is never erased. Every
  record chains to the previous one, so `zeus journal verify` names the first
  divergent record: the line whose `prev` no longer matches its predecessor.
  An edit to the last record has no successor to check it, so the chain proves
  ordering and continuity, not the final record's contents.
  `zeus journal resolve` closes an unmatched intent under the
  same lock instead of hand-editing the file. Malformed records, over-long
  lines and duplicate/regressed sequences fail loudly, never silently skipped.
  Journals from older builds without `prev` still open, with one warning that
  their chain is unverified.
- **Approval gate** — `--ask` (or `--approve`) parks the machine with the
  effect pending and writes a dossier; `zeus approve` checks the snapshot's own
  `(lineage, seq)` and the digest against the machine the dossier names, binds
  `(lineage, seq, digest)` to that exact machine, and `zeus deny` kills it. A
  journal that has moved past the dossier refuses the mismatch (exit 1) unless
  `--allow-gap` is given; a supplied approval is checked against the pending
  effect, so a spent one is a denial (exit 4). A snapshot is only resumable
  against the journal chain it was parked in. A
  machine with several sensitive effects parks once per effect. A malformed
  approval is an error, not a denial: the parked machine is left untouched.
- **OS jail** — `zeus jail` adds a network namespace, a uid drop and a seccomp
  filter that refuses raw sockets.

```bash
./zeus run-asm prog.zasm --ask --save m.zsnap --journal audit.jsonl
./zeus approve m.zsnap.dossier
./zeus restore m.zsnap --approve m.zsnap.dossier.approve --journal audit.jsonl

./zeus journal verify audit.jsonl
./zeus journal resolve --journal audit.jsonl --seq 7 \
    --outcome aborted --reason "worker died before the effect"
```

## Examples

| Example | Description |
|---------|-------------|
| `examples/hello.zasm` | Prints a greeting |
| `examples/echo_server.zasm` | TCP echo server on port 12345 |
| `examples/http_server.zasm` | Tiny HTTP server on port 8080 |
| `examples/raw_ping.zasm` | Allocates and sends a raw ICMP packet (requires root/CAP_NET_RAW and `--allow-raw`) |
| `examples/boot_splash.zasm` | Prints the Zeus_osVM ASCII logo at startup |

## Running Tests

```bash
make test
```

That is 4/4 VM, 3/3 assembler, 12/12 safety, 14/14 snapshot and 19/19
protection unit tests, then the 66-check approval flow, the 23-check deny
proof and the 20-check journal-integrity proof. `make check` reruns the same
suite under ASan + UBSan.

## Architecture

- `include/` public headers
- `src/` VM, effect policy, supervisor and journal, jail, assembler, disassembler, and CLI
- `mo/` Python spec runner (does not import the C VM)
- `examples/` sample Zeus assembly programs
- `tests/` C unit tests
- `assets/` project logo and ASCII boot splash
- `tools/` helper scripts (logo-to-ASCII generator, splash generator)

The VM uses separate operand and call stacks, a flat runtime memory, and a BSD-sockets-based networking layer.

## Opcode Summary

See `include/isa.h` for the full instruction set. Common opcodes:

- Stack: `PUSH`, `POP`, `DUP`, `SWAP`, `PICK`, `ROLL`
- Arithmetic: `ADD`, `SUB`, `MUL`, `DIV`, `MOD`, `NEG`
- Control: `JMP`, `JZ`, `JNZ`, `CALL`, `RET`, `HALT`
- Memory: `LOAD`, `STORE`, `LOADB`, `STOREB`, `LOADI`, `STOREI`, `LOADBI`, `STOREBI`
- I/O: `PRINT`, `PRINTC`, `READ`
- Networking: `SOCK_TCP`, `SOCK_UDP`, `BIND`, `LISTEN`, `ACCEPT`, `CONNECT`, `SEND`, `RECV`, `CLOSE`
- Raw packets: `RAW_ALLOC`, `RAW_FREE`, `RAW_SET_PROTO`, `RAW_SET_DST`, `RAW_SET_SRC`, `RAW_SET_PAYLOAD`, `RAW_SEND`, `RAW_RECV`

## MO — the Mount Olympus spec runner

`mo/` is a Python sibling package (it never imports the C VM). It judges an
agent's behavior against a declared `.zspec`: tracks open obligations, fires
verdicts — `FULFILLED`, `BOLTED` (a liveness window lapsed), `CONDEMNED` (a
safety assertion broke). See [`MO_PLAN.md`](MO_PLAN.md) for the design.

Five ways to run it:

```bash
python3 -m mo eval   spec.zspec trace.jsonl          # replay a recorded ZeusEvent stream
python3 -m mo eval   --mcp spec.zspec session.jsonl  # replay a captured Glassport MCP tap log
python3 -m mo watch  spec.zspec session.jsonl        # judge a growing tap log live (ticks expire silent windows)
python3 -m mo watch  --log-dir tap spec.zspec -- \
    glassport wrap --log-dir tap -- python server.py  # launch the tap and judge it in one command
python3 -m mo report spec.zspec trace.jsonl -o out.html  # render a self-contained HTML timeline
```

With `--log-dir DIR` and a command after `--`, MO snapshots the directory,
starts the command in its own process group, waits for the session log it
creates, and tails that log live. When the child exits, a short grace poll
drains its last records; when the judge stops first, the whole child group is
terminated. A missing log is exit 2, and a child that fails is a deviation
(exit 1). Demo: `python3 -m mo.examples.watch_launcher_demo`.

Specs compose: `INCLUDE fragments/mcp_liveness.zspec` merges another `.zspec`
at that point (relative to the including file, include-once). Every rule and
verdict keeps the fragment's file as `spec_source`, so a BOLT names where the
rule lived; single-file spec output is unchanged. Demo:
`python3 -m mo.examples.spec_fragments_demo --out fragments.html`.

The real-failure demo needs the `glassport` tap:

```bash
pip install glassport
python3 -m mo.examples.real_failure_demo --out real-failure.html
```

Verdicts go to stdout as JSONL, a human summary to stderr; exit code is `1` if
anything BOLTED or CONDEMNED, so `mo` works as a CI gate. Run the tests with
`python3 -m pytest mo/`; CI runs that suite in a separate `mo` job beside the
C `make check` job. Live `mo watch` derives its synthetic-tick interval from
the tightest declared window (100ms ceiling, 5ms floor, poll at half the tick),
so a short window is not noticed a full default tick late; `--tick-ms`
overrides it.

Replay determinism is checked statically: the judge core may read time only
from event timestamps, and `mo/tests/test_determinism.py` fails if any
`time`/`datetime` access reaches it.

A BOLT keeps `closed_at` as notice-time (the event that revealed the lapse)
and records the exact window boundary as `detail.lapsed_at`, plus
`detail.notice_lag_ms`. A late flush therefore cannot make a five-second
window look like a longer obligation, and the report tooltip shows the window,
the notice-time duration, the lapse instant and the lag.

## License

MIT — see [`LICENSE`](LICENSE). Have fun!
