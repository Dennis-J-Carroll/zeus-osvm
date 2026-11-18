#ifndef ZEUS_SNAPSHOT_H
#define ZEUS_SNAPSHOT_H

#include "zeus.h"
#include "vm.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Snapshot contract
 * -----------------
 * A snapshot is the whole machine: program, registers, both stacks, memory,
 * raw-packet slots (including their slot ids), and any pending effect.
 *
 * Writer: zeus_vm_snapshot()
 *  - `vm` may be running, yielded or halted, but must hold a loaded program.
 *  - The writer refuses (ZEUS_ERR_SNAPSHOT) to serialize an inconsistent
 *    machine: bad pc/sp/csp, running without started or with an error, a
 *    yielded machine with no pending effect, effect operands outside memory,
 *    or a malformed packet slot. Every file written is therefore a state the
 *    VM could actually have produced.
 *  - The same machine state always serializes to identical bytes.
 *
 * Reader: zeus_vm_restore()
 *  - `vm` must have been initialized with zeus_vm_init(). It may already hold
 *    a program; that state is released on success.
 *  - Restore is atomic: the file is fully parsed and validated into a
 *    temporary machine first. On any failure `*vm` is left exactly as it was
 *    and ZEUS_ERR_SNAPSHOT (or ZEUS_ERR_IO) is returned. On success `*vm` is
 *    replaced wholesale.
 *  - Validation covers the FNV-1a checksum, both format and bytecode
 *    versions, the full header, and the invariants execution relies on:
 *    running implies started and error == ZEUS_OK; yielded implies running
 *    and a non-NONE effect; a non-yielded machine has a zeroed pending
 *    effect; pending buffers and strings lie inside memory; packet slots are
 *    well formed and keep their ids.
 *  - Lifecycle state is preserved exactly. Restore does not start a halted
 *    machine and does not consume a pending effect; the next
 *    zeus_vm_step/zeus_vm_resume sees the same state the writer did.
 *  - Restoring twice from one file yields two independent machines: that is
 *    the fork primitive, with no extra code.
 */

ZeusError zeus_vm_snapshot(const ZeusVM *vm, const char *path);

/* Initialize `vm` from a snapshot file. Requires zeus_vm_init(&vm) first.
 * Leaves `vm` untouched on failure. */
ZeusError zeus_vm_restore(ZeusVM *vm, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* ZEUS_SNAPSHOT_H */
