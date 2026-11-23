# Zeus_osVM

![Zeus_osVM logo](assets/logo.jpg)

A fun, educational stack-based virtual machine written in C, designed for experimenting with networking concepts.

## Features

- Stack-based bytecode interpreter with 64-bit words
- Suspendable machines: `zeus save` / `zeus restore` snapshot the entire ~132 KiB machine, pending effect and all
- First-class socket opcodes: `SOCK_TCP`, `BIND`, `LISTEN`, `ACCEPT`, `CONNECT`, `SEND`, `RECV`, `CLOSE`
- Raw packet opcodes: `RAW_ALLOC`, `RAW_SET_PROTO`, `RAW_SET_DST`, `RAW_SET_SRC`, `RAW_SET_PAYLOAD`, `RAW_SEND`, `RAW_RECV`
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

Every external operation is an effect the interpreter yields rather than
performs, so a stopped machine is just its state. Run a fixed number of
instructions, write the machine to disk, and pick it up later — possibly more
than once:

```bash
./zeus run-asm examples/hello.zasm --max-steps 3 --save m.zsnap
./zeus restore m.zsnap          # continues exactly where it stopped
```

Restoring the same snapshot twice gives two independent machines. Snapshots
are versioned and checksummed.

## Protection

The interpreter never touches the host directly, which makes the effect seam a
natural policy point:

- **Default-deny policy** — console and sockets are allowed; `RAW_*` effects
  need `--allow-raw`. A denied effect halts the machine (exit 4) instead of
  handing the program a failure result it can ignore.
- **Append-only audit** — `--journal F` writes `intent` before each effect and
  `commit` after, with a supervisor-owned sequence number and a digest of the
  resolved operands. The journal is held under an exclusive lock for the life
  of the supervisor, so two processes cannot scan the same counter and mint the
  same number. A crash between intent and commit is refused on every retry: a
  refused open writes nothing, so the unmatched intent is never erased.
- **Approval gate** — `--ask` (or `--approve`) parks the machine with the
  effect pending and writes a dossier; `zeus approve` checks the digest against
  the snapshot the dossier names, binds `(lineage, seq, digest)` to that exact
  machine, and `zeus deny` kills it. A
  machine with several sensitive effects parks once per effect. A malformed
  approval is an error, not a denial: the parked machine is left untouched.
- **OS jail** — `zeus jail` adds a network namespace, a uid drop and a seccomp
  filter that refuses raw sockets.

```bash
./zeus run-asm prog.zasm --ask --save m.zsnap --journal audit.jsonl
./zeus approve m.zsnap.dossier
./zeus restore m.zsnap --approve m.zsnap.dossier.approve --journal audit.jsonl
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

## Architecture

- `include/` public headers
- `src/` VM, assembler, disassembler, and CLI
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

## MO — the Mount Olympus spec runner

`mo/` is a Python sibling package (it never imports the C VM). It judges an
agent's behavior against a declared `.zspec`: tracks open obligations, fires
verdicts — `FULFILLED`, `BOLTED` (a liveness window lapsed), `CONDEMNED` (a
safety assertion broke). See [`MO_PLAN.md`](MO_PLAN.md) for the design.

Four ways to run it:

```bash
python3 -m mo eval   spec.zspec trace.jsonl          # replay a recorded ZeusEvent stream
python3 -m mo eval   --mcp spec.zspec session.jsonl  # replay a captured Glassport MCP tap log
python3 -m mo watch  spec.zspec session.jsonl        # judge a growing tap log live (ticks expire silent windows)
python3 -m mo report spec.zspec trace.jsonl -o out.html  # render a self-contained HTML timeline
```

Verdicts go to stdout as JSONL, a human summary to stderr; exit code is `1` if
anything BOLTED or CONDEMNED, so `mo` works as a CI gate. Run the tests with
`python3 -m pytest mo/`.

## License

MIT — see [`LICENSE`](LICENSE). Have fun!
