#ifndef ZEUS_SUPERVISOR_H
#define ZEUS_SUPERVISOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "vm.h"
#include "policy.h"
#include "snapshot.h"

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

/* The one audit-id rule, shared by the supervisor and the journal parser:
 * exactly 32 hex characters (0-9, a-f, A-F). */
bool zeus_valid_id(const char *s);

typedef enum {
    ZEUS_DRIVE_HALTED = 0,     /* program halted cleanly */
    ZEUS_DRIVE_STEP_LIMIT,     /* stopped at the instruction budget */
    ZEUS_DRIVE_NEEDS_APPROVAL, /* parked with a pending effect */
    ZEUS_DRIVE_DENIED,         /* policy or approval refused the effect */
    ZEUS_DRIVE_DENY_UNRECORDED,/* the effect was refused but the journal
                                * could not record the refusal */
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

/* Result of `zeus journal verify`. `line` is the first divergent record (0
 * when the chain is clean). A legacy journal is structurally valid but its
 * chain cannot be verified; --allow-legacy turns that into a warning. */
typedef struct {
    uint64_t line;
    uint64_t records;
    bool legacy;
    char reason[256];
} ZeusJournalVerify;

typedef struct {
    FILE *journal;
    bool journal_busy;         /* open failed: another supervisor holds the lock */
    ZeusPolicy policy;
    char machine_id[ZEUS_ID_LEN];
    char lineage_id[ZEUS_ID_LEN];
    uint64_t seq;              /* last committed effect number */
    bool ambiguous;            /* journal ends in an unmatched intent */
    uint64_t ambiguous_seq;
    uint64_t head_hash;        /* FNV-1a of the last journal record */
    bool chain_unverified;     /* journal has legacy records without prev */
    char journal_error[256];   /* why open refused, when it was not the lock */
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
 * cannot ignore the refusal and continue. A recorded refusal spends its
 * sequence number, so a denied effect can never later be approved under the
 * same seq. Returns whether the refusal reached the journal; the effect is
 * cancelled and the machine halted either way, and an unrecorded refusal does
 * not advance the counter (the journal's last committed number stays true). */
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

/* True only if the snapshot's binding metadata may back the machine a dossier
 * describes. An unbound snapshot (empty lineage, seq 0) can never match a
 * dossier, whose sequence numbers start at 1. On mismatch `why` explains what
 * disagreed; it is left untouched on success. */
bool zeus_snapshot_matches_dossier(const ZeusSnapshotMeta *m,
                                   const ZeusDossier *d,
                                   char *why, size_t why_len);

/* Walk a journal's hash chain and sequence accounting. Returns true when the
 * chain is clean (or legacy records are accepted with `allow_legacy`). On
 * failure `out->line` names the first divergent record and `out->reason` says
 * what broke. `lineage` restricts the sequence checks to one audit chain. */
bool zeus_journal_verify(const char *path, const char *lineage,
                         bool allow_legacy, ZeusJournalVerify *out);

/* Close an open intent: appends a `resolve` record under the same exclusive
 * lock, spending `seq`. Refuses when there is no open intent at `seq` or when
 * more than one lineage has one. */
bool zeus_journal_resolve(const char *path, uint64_t seq,
                          const char *outcome, const char *reason,
                          char *why, size_t why_len);

/* True only when `head` occurs in the journal's hash chain. Snapshot writes
 * record the head at the park record; restore and deny use this to refuse a
 * machine parked against a different journal. `head` 0 means unbound. */
bool zeus_journal_contains_head(const char *path, uint64_t head, bool *found,
                                char *why, size_t why_len);

#ifdef __cplusplus
}
#endif

#endif /* ZEUS_SUPERVISOR_H */
