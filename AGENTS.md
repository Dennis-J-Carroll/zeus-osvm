# Agent Notes for Zeus_osVM

## Build & Test

- Build: `make` (produces `zeus`, `zasm`, `zdis`)
- Test: `make test`
- Clean: `make clean`

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

## Adding New Instructions

1. Add opcode constant to `include/isa.h`
2. Add metadata to `src/isa.c`
3. Implement execution in `src/vm.c`
4. Add assembler support if it takes an operand (already handled generically by `has_operand`)
5. Add a test to `tests/test_vm.c`

## Raw Networking Notes

Raw packet opcodes create IP-layer raw sockets. On Linux this requires `CAP_NET_RAW` or root. The VM compiles without privileges but raw operations return `-1` at runtime if denied.
