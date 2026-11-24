#ifndef ZEUS_SNAPSHOT_H
#define ZEUS_SNAPSHOT_H

#include "zeus.h"
#include "vm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZEUS_SNAPSHOT_LINEAGE_LEN 33      /* 32 hex chars + NUL */

/*
 * Binding metadata stored in a version-3 snapshot.
 *
 * `lineage` is the audit lineage whose accounting the parked machine belongs
 * to (empty when the snapshot is unbound), `seq` is the effect number that was
 * pending when it was written (0 when unbound), and `journal_head` is reserved
 * for the journal chain hash (written as 0 until the chain lands).
 *
 * The binding is what stops a dossier from being paired with a different
 * machine: approve/deny restore the snapshot and compare this metadata with
 * the dossier before trusting the digest.
 */
typedef struct {
    char lineage[ZEUS_SNAPSHOT_LINEAGE_LEN];
    uint64_t seq;
    uint64_t journal_head;
} ZeusSnapshotMeta;

/*
 * Snapshot contract
 * -----------------
 * A snapshot is the whole machine: program, registers, both stacks, memory,
 * raw-packet slots (including their slot ids), and any pending effect.
 *
 * Writer: zeus_vm_snapshot() / zeus_vm_snapshot_bound()
 *  - `vm` may be running, yielded or halted, but must hold a loaded program.
 *  - The writer refuses (ZEUS_ERR_SNAPSHOT) to serialize an inconsistent
 *    machine: bad pc/sp/csp, running without started or with an error, a
 *    yielded machine with no pending effect, effect operands outside memory,
 *    or a malformed packet slot. Every file written is therefore a state the
 *    VM could actually have produced.
 *  - The same machine state always serializes to identical bytes.
 *  - Writes are atomic: the file is written to a same-directory temporary,
 *    fsynced, renamed over the destination, and the parent directory is
 *    fsynced. A failed save leaves the previous file byte-identical and no
 *    temporary behind.
 *  - `zeus_vm_snapshot()` is the unbound form (empty lineage, seq 0).
 *    `zeus_vm_snapshot_bound()` records the audit identity a parked machine
 *    must keep; `lineage` must be empty or a 32-hex id.
 *
 * Reader: zeus_vm_restore() / zeus_vm_restore_ex()
 *  - `vm` must have been initialized with zeus_vm_init(). It may already hold
 *    a program; that state is released on success.
 *  - Restore is atomic: the file is fully parsed and validated into a
 *    temporary machine first. On any failure `*vm` is left exactly as it was
 *    and ZEUS_ERR_SNAPSHOT (or ZEUS_ERR_IO) is returned. On success `*vm` is
 *    replaced wholesale.
 *  - Validation covers the FNV-1a checksum, both format and bytecode
 *    versions, the full header, the binding metadata, and the invariants
 *    execution relies on: running implies started and error == ZEUS_OK;
 *    yielded implies running and a non-NONE effect; a non-yielded machine has
 *    a zeroed pending effect; pending buffers and strings lie inside memory;
 *    packet slots are well formed and keep their ids.
 *  - `zeus_vm_restore_ex()` reports the binding metadata on success; `out` is
 *    written only when ZEUS_OK is returned.
 *  - Lifecycle state is preserved exactly. Restore does not start a halted
 *    machine and does not consume a pending effect; the next
 *    zeus_vm_step/zeus_vm_resume sees the same state the writer did.
 *  - Restoring twice from one file yields two independent machines: that is
 *    the fork primitive, with no extra code.
 */

ZeusError zeus_vm_snapshot(const ZeusVM *vm, const char *path);

/* Write a v3 snapshot bound to (lineage, seq). `lineage` is empty or a
 * 32-hex audit id; anything else is refused with ZEUS_ERR_SNAPSHOT. */
ZeusError zeus_vm_snapshot_bound(const ZeusVM *vm, const char *path,
                                 const char *lineage, uint64_t seq);

/* Bound writer that also records the journal head the machine was parked at.
 * Restore validates that head against the journal given to the run; 0 means
 * unbound. zeus_vm_snapshot_bound() passes 0 here. */
ZeusError zeus_vm_snapshot_bound_ex(const ZeusVM *vm, const char *path,
                                    const char *lineage, uint64_t seq,
                                    uint64_t journal_head);

/* Initialize `vm` from a snapshot file. Requires zeus_vm_init(&vm) first.
 * Leaves `vm` untouched on failure. */
ZeusError zeus_vm_restore(ZeusVM *vm, const char *path);

/* Restore and, on success, report the snapshot's binding metadata. Requires
 * zeus_vm_init(&vm) first; leaves `vm` untouched on failure. */
ZeusError zeus_vm_restore_ex(ZeusVM *vm, const char *path,
                             ZeusSnapshotMeta *out);

#ifdef __cplusplus
}
#endif

#endif /* ZEUS_SNAPSHOT_H */
