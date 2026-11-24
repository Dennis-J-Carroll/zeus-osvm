#ifndef ZEUS_JOURNAL_H
#define ZEUS_JOURNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Internal journal seam shared by src/supervisor.c and src/journal.c. The
 * public verify/resolve entry points live in supervisor.h. */

#define ZEUS_JOURNAL_LINE_MAX 4096u
#define ZEUS_JOURNAL_ID_LEN   33

typedef struct {
    uint64_t max_seq;          /* last committed/resolved seq for the lineage */
    bool ambiguous;            /* an intent is still open */
    uint64_t ambiguous_seq;
    uint64_t head_hash;        /* FNV-1a of the last record (0 if empty) */
    bool chain_unverified;     /* at least one record has no prev */
} ZeusJournalScan;

/* FNV-1a over a record's bytes (the line without its trailing newline). */
uint64_t zeus_journal_hash(const void *p, size_t n);

/* Parse `times` records and return the counter state for `lineage`
 * (NULL = validate all lineages, max_seq is then unused). Fails loudly on a
 * malformed record, an over-long line, a truncated record, a chain break or a
 * duplicate/regressed sequence; `why` names the first offending line. */
bool zeus_journal_scan(const char *path, const char *lineage,
                       ZeusJournalScan *out, char *why, size_t why_len);

/* Append one already-built record (no newline) with write(2) + fsync, rolling
 * back a short append. The caller owns the lock and the prev field. */
bool zeus_journal_append_fd(int fd, const char *record, size_t len);

/* Escape `src` for a JSON string. Refuses when the escaped form cannot fit. */
bool zeus_json_escape(const char *src, char *dst, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ZEUS_JOURNAL_H */
