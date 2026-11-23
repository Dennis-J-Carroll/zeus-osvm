#ifndef ZEUS_SUPERVISOR_H
#define ZEUS_SUPERVISOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "vm.h"
#include "policy.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Supervisor-owned accounting
 * ---------------------------
 * The supervisor is the process, not the machine. It holds the policy, the
 * audit journal and the effect counter. None of that lives in a ZeusVM and
 * none of it is written into a snapshot, so restoring a file cannot rewind the
 * counter, erase a denial or loosen the policy.
 *
 * Ordering rule: an `intent` record is flushed and fsynced BEFORE the effect
 * reaches the host, and a `commit` record follows. A crash therefore leaves at
 * most one unmatched intent, which the next open reports as ambiguous and
 * refuses to run through: an effect that may already have happened is never
 * silently attempted twice.
 */

#define ZEUS_ID_LEN   33      /* 32 hex chars + NUL */
#define ZEUS_PATH_LEN 1024

typedef enum {
    ZEUS_DRIVE_HALTED = 0,     /* program halted cleanly */
    ZEUS_DRIVE_STEP_LIMIT,     /* stopped at the instruction budget */
    ZEUS_DRIVE_NEEDS_APPROVAL, /* parked with a pending effect */
    ZEUS_DRIVE_DENIED,         /* policy or approval refused the effect */
    ZEUS_DRIVE_FAILED          /* VM error, I/O error or ambiguous journal */
} ZeusDriveResult;

/* What the operator is asked to approve. `seq` is the supervisor's next
 * effect number; `digest` covers the resolved operands (including buffer
 * contents), so approving it approves this exact effect, not a file name. */
typedef struct {
    char lineage[ZEUS_ID_LEN];
    uint64_t seq;
    uint64_t digest;
    char effect[24];
    char snapshot[ZEUS_PATH_LEN];
} ZeusDossier;

typedef struct {
    char lineage[ZEUS_ID_LEN];
    uint64_t seq;
    uint64_t digest;
} ZeusApproval;

typedef struct {
    FILE *journal;
    bool journal_busy;         /* open failed: another supervisor holds the lock */
    ZeusPolicy policy;
    char machine_id[ZEUS_ID_LEN];
    char lineage_id[ZEUS_ID_LEN];
    uint64_t seq;              /* last committed effect number */
    bool ambiguous;            /* journal ends in an unmatched intent */
    uint64_t ambiguous_seq;
    bool ask;                  /* park instead of delivering sensitive effects */

    bool have_approval;        /* an approval is bound to the next effect */
    ZeusApproval approval;     /* parsed once; the file is never re-read */

    char snapshot_path[ZEUS_PATH_LEN];
    char dossier_path[ZEUS_PATH_LEN];
} ZeusSupervisor;

void zeus_supervisor_init(ZeusSupervisor *s);

/* Open a supervisor. `lineage` NULL/empty starts a new audit lineage; pass an
 * existing id to continue one (effect numbers resume from the journal).
 * `journal_path` NULL disables auditing (accounting stays in memory). */
bool zeus_supervisor_open(ZeusSupervisor *s, const ZeusPolicy *policy,
                          const char *journal_path, const char *lineage);

void zeus_supervisor_close(ZeusSupervisor *s);

/* Paths are copied; the dossier is always "<snapshot>.dossier". */
void zeus_supervisor_set_paths(ZeusSupervisor *s, const char *snapshot_path);

/* Bind an already validated approval. It authorises one effect and is held in
 * memory, so a file that is changed or truncated after validation cannot turn
 * into a denial at the effect. Pass NULL to unbind. */
void zeus_supervisor_bind_approval(ZeusSupervisor *s, const ZeusApproval *a);

uint64_t zeus_supervisor_next_seq(const ZeusSupervisor *s);

/* FNV-1a over the effect fields plus any memory the effect addresses. This is
 * the operand digest an approval binds to. */
uint64_t zeus_effect_digest(const ZeusVM *vm, const ZeusEffect *eff);

/* Drive the machine until it halts, hits `max_steps`, parks or is refused.
 * Effects are checked against the policy before delivery; `why` receives a
 * human-readable reason on denial/failure. */
ZeusDriveResult zeus_supervisor_drive(ZeusSupervisor *s, ZeusVM *vm,
                                      long max_steps,
                                      char *why, size_t why_len);

/* Operator denial: journal the effect at its next sequence number, then kill
 * the machine at the seam. The effect is never delivered, so a denied program
 * cannot ignore the refusal and continue. The refusal spends its sequence
 * number, so a denied effect can never later be approved under the same seq.
 * Returns false if the denial could not be journaled; the machine is halted
 * either way. */
bool zeus_supervisor_deny(ZeusSupervisor *s, ZeusVM *vm, const char *reason);

/* Dossier / approval files. Approval is keyed by (lineage, seq, digest) and is
 * verified against the machine in hand, never against a path. */
bool zeus_supervisor_write_dossier(const ZeusSupervisor *s, const ZeusVM *vm,
                                   const ZeusEffect *eff,
                                   const char *dossier_path);
bool zeus_dossier_load(const char *path, ZeusDossier *out);

bool zeus_approval_write(const ZeusDossier *d, const char *path);
bool zeus_approval_load(const char *path, ZeusApproval *out);

/* True only if `a` approves exactly this pending effect of this machine. */
bool zeus_approval_binds(const ZeusApproval *a, const ZeusSupervisor *s,
                         const ZeusVM *vm, const ZeusEffect *eff);

#ifdef __cplusplus
}
#endif

#endif /* ZEUS_SUPERVISOR_H */
