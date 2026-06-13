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

## Verified Working

`make` builds all three binaries; `make test` passes 7/7. `hello`, `echo_server`, `http_server`, `boot_splash` examples run; `raw_ping` needs `CAP_NET_RAW`/root.

## Note on AGENTS.md

`AGENTS.md` holds the same notes for other agent tools. Both files are maintained independently — **update both when changing build/layout/conventions** to avoid drift.
