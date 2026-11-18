# Agent Notes for Zeus_osVM

## Build & Test

- Build: `make` (produces `zeus`, `zasm`, `zdis`)
- Test: `make test`
- Sanitizers: `make check` (ASan + UBSan over the whole suite, then a normal rebuild)
- Smoke test: `make smoke` (builds + runs `./smoke_test.sh` end-to-end against all examples)
- Clean: `make clean` (also removes `examples/*.zeus` and the `zasm`/`zdis` wrappers)

## CLI (src/main.c)

The `zeus` binary dispatches on `argv[1]`:

```bash
zeus run prog.zeus            # run a bytecode file
zeus restore m.zsnap          # resume a suspended machine
zeus save prog.zeus m.zsnap   # snapshot a machine (default: at entry)
zeus asm in.zasm out.zeus     # assemble .zasm -> .zeus
zeus dis prog.zeus            # disassemble bytecode
zeus run-asm in.zasm          # assemble + run in one step
zeus approve d.dossier        # approve the exact pending effect
zeus deny d.dossier           # kill a parked machine at its effect
zeus jail -- ./zeus run p.zeus # namespaces + uid drop + seccomp
```

`run`, `restore` and `run-asm` accept `--max-steps N` (stop after N
instructions and leave the machine suspended), `--journal F` (append-only JSONL
audit log: `intent` before each effect, `commit` after), `--save F` (snapshot on
exit; with `--ask`, the parking file), `--ask` (park for approval), `--approve F`
(check an approval against the pending effect), `--allow-raw` (allow `RAW_*`)
and `--lineage ID` (continue an audit lineage). Exit codes: 0 ok, 1 error,
3 parked, 4 denied.

`zasm` and `zdis` are generated shell wrappers that `exec ./zeus asm` / `./zeus dis`. They are recreated by `make` and deleted by `make clean`.

## Docs

`docs/USAGE.md` — comprehensive usage + full opcode reference (verified against source). `README.md` for the human overview.

## Project Layout

- `include/` - public C headers
- `src/` - implementation
  - `zeus.c`, `isa.c`, `bytecode.c` - common utilities
  - `vm.c` - interpreter core + effect yields
  - `snapshot.c` - versioned, checksummed machine save/restore
  - `policy.c` - default-deny effect policy (supervisor-owned)
  - `supervisor.c` - append-only journal, effect accounting, approval gate
  - `jail.c` - namespaces, uid drop, seccomp filter
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

Raw packet opcodes create IP-layer raw sockets. On Linux this requires `CAP_NET_RAW` or root. The VM compiles without privileges but raw operations return `-1` at runtime if denied. Since the protection layer landed, `RAW_*` is denied by default: pass `--allow-raw`, and note that `zeus jail`'s seccomp filter refuses raw socket creation outright.

## Gotchas & Latent Capabilities

- **Bytecode header:** `.zeus` files start with `ZEUS_MAGIC` + `ZEUS_VERSION` (`src/bytecode.c`, `include/bytecode.h`). Loader rejects mismatched magic/version — bump `ZEUS_VERSION` when changing the format.
- **Assembler string limit:** single-line string literals are bounded by an 8192-byte buffer (`src/asm.c:257`). Long strings (e.g. boot splash) must stay under it.
- **Effects, not calls:** the interpreter never blocks. Sockets, raw packets, `READ`, `PRINT` and `PRINTC` yield a `ZeusEffect` (`include/vm.h`); `zeus_vm_exec` + `zeus_vm_resume` expose that seam, `zeus_vm_step` keeps the old "just do it" behaviour. Anything you add that touches the host must go through this seam or snapshots stop being faithful.
- **Snapshot contract:** the writer refuses inconsistent machines; `zeus_vm_restore` requires a `zeus_vm_init`-ed VM, validates the whole file into a temporary machine, and replaces the machine atomically. A failed restore leaves the old machine intact. Raw-packet slot ids are preserved, so a fork keeps packet ids stable.
- **Machine lifecycle:** a loaded machine is not started. Call `zeus_vm_start()` (or `zeus_vm_run()`) before stepping it.
- **Arithmetic is total:** wrap at 64 bits, mask shift counts to 0–63, and `INT64_MIN / -1` wraps to `INT64_MIN`. Never reintroduce signed overflow — replay and snapshots depend on defined results.
- **Policy is supervisor-owned:** `ZeusPolicy` (`include/policy.h`) is applied by `src/supervisor.c` before delivery, never by the VM. The default profile is STANDARD (console + sockets, `raw=false`); a denial cancels the effect, halts the machine and is journaled. Never move policy into `ZeusVM` or a snapshot.
- **Accounting is supervisor-owned:** the effect counter, `machine_id`/`lineage_id` and the journal live outside the machine, so a snapshot cannot rewind them. `intent` is fsynced before delivery; an unmatched intent makes the next run on that lineage refuse to start.
- **Approval binds the machine, not the file:** an approval is `(lineage, seq, digest)` and the digest covers resolved operands, including buffer contents. `--approve` is verified against the loaded machine; one approval authorises one effect; `--ask` and `--approve` both imply the gate and need `--journal` (a restore without `--save` parks back into the file it came from, so several sensitive effects park one at a time). A refusal spends its sequence number, so a denied effect cannot later be approved under the same seq.
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

`make` builds all three binaries; `make test` passes 4/4 + 3/3 + 12/12 + 10/10 + 6/6 (VM, assembler, safety, snapshot, protection) plus the 7-check `tests/test_ask.sh` approval flow; `make check` passes under ASan + UBSan. `make smoke` passes 8/8. `hello`, `echo_server`, `http_server`, `boot_splash` examples run; `raw_ping` needs `CAP_NET_RAW`/root and `--allow-raw`. MO: `python3 -m pytest mo/` passes 65, 2 skipped (the live dogfood is marked `slow`). `zeus jail --check` reports seccomp working (raw sockets denied with EPERM); the namespace layer depends on the host allowing unprivileged user namespaces and fails closed when it does not.

Recent additions:
- Correlation ids: `ZeusEvent` and `Obligation` carry a `correlation` field so
  concurrent calls to the same tool close the right obligation (premortem #2).
- Live dogfood verified: `glassport wrap -- python3 fake_server.py` + `mo watch`
  on the growing tap log produced one `FULFILLED` verdict and no deviations.

## Note on CLAUDE.md

`CLAUDE.md` holds the same notes for Claude Code. Both files are maintained independently — **update both when changing build/layout/conventions** to avoid drift.
