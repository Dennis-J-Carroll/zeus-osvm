#define _POSIX_C_SOURCE 200809L

#include "supervisor.h"
#include "snapshot.h"
#include "journal.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Small utilities                                                     */
/* ------------------------------------------------------------------ */

static uint64_t fnv_bytes(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}

static uint64_t fnv_u64(uint64_t h, uint64_t v) {
    uint8_t p[8];
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
    return fnv_bytes(h, p, sizeof(p));
}

static void fill_random(uint8_t *out, size_t n) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        size_t got = fread(out, 1, n, f);
        fclose(f);
        if (got == n) return;
    }
    /* Fallback for hosts without /dev/urandom: still unique enough to keep
     * audit records from colliding. Not a security primitive. */
    uint64_t seed = (uint64_t)time(NULL)
                  ^ ((uint64_t)getpid() << 32)
                  ^ (uint64_t)(uintptr_t)out;
    for (size_t i = 0; i < n; i++) {
        seed = seed * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
        out[i] = (uint8_t)(seed >> 33);
    }
}

static void make_id(char out[ZEUS_ID_LEN]) {
    uint8_t b[16];
    fill_random(b, sizeof(b));
    for (int i = 0; i < 16; i++) {
        static const char hex[] = "0123456789abcdef";
        out[i * 2] = hex[(b[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[b[i] & 0xF];
    }
    out[32] = '\0';
}

bool zeus_valid_id(const char *s) {
    if (!s || strlen(s) != 32) return false;
    for (int i = 0; i < 32; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

static void set_why(char *why, size_t n, const char *fmt, ...) {
    if (!why || n == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, n, fmt, ap);
    va_end(ap);
}

static bool range_ok(int64_t addr, int64_t bytes) {
    if (addr < 0 || bytes < 0) return false;
    if ((uint64_t)addr > ZEUS_MEMORY_SIZE) return false;
    return (uint64_t)bytes <= (uint64_t)ZEUS_MEMORY_SIZE - (uint64_t)addr;
}

static size_t bounded_strlen(const uint8_t *p, size_t max) {
    size_t i = 0;
    while (i < max && p[i] != '\0') i++;
    return i;
}

/* ------------------------------------------------------------------ */
/* Journal                                                             */
/* ------------------------------------------------------------------ */

static bool journal_write(ZeusSupervisor *s, const char *line) {
    if (!s->journal) return true;
    /* Nothing may follow an unmatched intent: any later record would look
     * like an outcome for the effect and let the next open replay it. An
     * ambiguous journal stays ambiguous until an operator resolves it. */
    if (s->ambiguous) return false;

    int fd = fileno(s->journal);
    if (fd < 0) return false;

    /* Every record names the hash of the previous one. Prepending it here keeps
     * each writer honest and gives the whole file one chain. */
    char buf[ZEUS_JOURNAL_LINE_MAX];
    int n = snprintf(buf, sizeof(buf), "{\"prev\":\"%016llx\",%s",
                     (unsigned long long)s->head_hash, line + 1);
    if (n < 0 || (size_t)n >= sizeof(buf)) return false;
    if (!zeus_journal_append_fd(fd, buf, (size_t)n)) return false;

    s->head_hash = zeus_journal_hash(buf, (size_t)n);
    return true;
}

static bool journal_start(ZeusSupervisor *s) {
    char line[512];
    snprintf(line, sizeof(line),
             "{\"seq\":0,\"event\":\"start\",\"machine\":\"%s\","
             "\"lineage\":\"%s\",\"raw\":%d}",
             s->machine_id, s->lineage_id, s->policy.allow_raw ? 1 : 0);
    return journal_write(s, line);
}

static bool journal_intent(ZeusSupervisor *s, uint64_t seq,
                           const ZeusEffect *e, uint64_t digest) {
    char line[1024];
    snprintf(line, sizeof(line),
             "{\"seq\":%llu,\"event\":\"intent\",\"machine\":\"%s\","
             "\"lineage\":\"%s\",\"effect\":\"%s\",\"fd\":%lld,\"id\":%lld,"
             "\"value\":%lld,\"buf\":%u,\"len\":%u,\"digest\":\"0x%016llx\"}",
             (unsigned long long)seq, s->machine_id, s->lineage_id,
             zeus_effect_name(e->kind), (long long)e->fd, (long long)e->id,
             (long long)e->value, e->buf, e->len,
             (unsigned long long)digest);
    return journal_write(s, line);
}

static bool journal_commit(ZeusSupervisor *s, uint64_t seq,
                           int64_t result, ZeusError err) {
    char line[512];
    snprintf(line, sizeof(line),
             "{\"seq\":%llu,\"event\":\"commit\",\"machine\":\"%s\","
             "\"lineage\":\"%s\",\"result\":%lld,\"error\":%d}",
             (unsigned long long)seq, s->machine_id, s->lineage_id,
             (long long)result, (int)err);
    return journal_write(s, line);
}

static bool journal_park(ZeusSupervisor *s, uint64_t pending_seq,
                         const ZeusEffect *e, uint64_t digest) {
    char line[1024];
    /* "seq" stays the last committed number so the scanner keeps it: the
     * pending number is data, not a sequence the journal has reached. */
    snprintf(line, sizeof(line),
             "{\"seq\":%llu,\"event\":\"park\",\"machine\":\"%s\","
             "\"lineage\":\"%s\",\"pending\":%llu,\"effect\":\"%s\","
             "\"digest\":\"0x%016llx\"}",
             (unsigned long long)s->seq, s->machine_id, s->lineage_id,
             (unsigned long long)pending_seq, zeus_effect_name(e->kind),
             (unsigned long long)digest);
    return journal_write(s, line);
}

static bool journal_deny(ZeusSupervisor *s, uint64_t seq, const ZeusEffect *e,
                         uint64_t digest, const char *reason) {
    char escaped[512];
    if (!zeus_json_escape(reason, escaped, sizeof(escaped))) return false;

    char line[1024];
    snprintf(line, sizeof(line),
             "{\"seq\":%llu,\"event\":\"deny\",\"machine\":\"%s\","
             "\"lineage\":\"%s\",\"effect\":\"%s\",\"digest\":\"0x%016llx\","
             "\"reason\":\"%s\"}",
             (unsigned long long)seq, s->machine_id, s->lineage_id,
             zeus_effect_name(e->kind), (unsigned long long)digest, escaped);
    return journal_write(s, line);
}

static bool journal_halt(ZeusSupervisor *s, const ZeusVM *vm, const char *reason) {
    char line[512];
    snprintf(line, sizeof(line),
             "{\"seq\":%llu,\"event\":\"halt\",\"machine\":\"%s\","
             "\"lineage\":\"%s\",\"error\":%d,\"pc\":%u,\"reason\":\"%s\"}",
             (unsigned long long)s->seq, s->machine_id, s->lineage_id,
             (int)vm->error, vm->pc, reason);
    return journal_write(s, line);
}

/* Refuse the pending effect: journal the refusal at `seq`, spend the number
 * and kill the machine at the seam. Spending it matters: a denial is an
 * outcome for that slot, so a later approval naming the same seq must not be
 * able to deliver an effect the journal already records as refused.
 *
 * The cancellation happens whether or not the append succeeded: an effect must
 * never reach the host after its refusal failed to become durable. Only a
 * recorded refusal spends the number, because the counter must agree with what
 * the journal can prove; an unrecorded one leaves the journal's last committed
 * seq untouched and says so through the return value. */
static bool deny_effect(ZeusSupervisor *s, ZeusVM *vm, uint64_t seq,
                        const ZeusEffect *eff, uint64_t digest,
                        const char *reason) {
    bool journaled = journal_deny(s, seq, eff, digest, reason);
    if (journaled) s->seq = seq;
    zeus_vm_cancel_effect(vm, ZEUS_ERR_EFFECT);
    return journaled;
}

static bool parse_u64_span(const char *s, uint64_t *out, const char **endp) {
    if (!s || *s < '0' || *s > '9') return false;
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno != 0 || end == s) return false;
    *out = (uint64_t)v;
    if (endp) *endp = end;
    return true;
}

/* The whole value must be digits: "7x" is a malformed field, not the
 * number 7. */
static bool parse_u64(const char *s, uint64_t *out) {
    const char *end = NULL;
    if (!parse_u64_span(s, out, &end)) return false;
    return end != NULL && *end == '\0';
}


/* ------------------------------------------------------------------ */
/* Digest                                                              */
/* ------------------------------------------------------------------ */

uint64_t zeus_effect_digest(const ZeusVM *vm, const ZeusEffect *eff) {
    uint64_t h = UINT64_C(1469598103934665603);
    if (!eff) return h;

    h = fnv_u64(h, (uint64_t)eff->kind);
    h = fnv_u64(h, (uint64_t)eff->fd);
    h = fnv_u64(h, (uint64_t)eff->id);
    h = fnv_u64(h, (uint64_t)eff->value);
    h = fnv_u64(h, (uint64_t)eff->buf);
    h = fnv_u64(h, (uint64_t)eff->len);

    if (!vm) return h;

    switch (eff->kind) {
        case ZEUS_EFFECT_CONNECT: {
            if (eff->buf < ZEUS_MEMORY_SIZE) {
                const uint8_t *p = vm->memory + eff->buf;
                size_t max = ZEUS_MEMORY_SIZE - (size_t)eff->buf;
                size_t n = bounded_strlen(p, max);
                if (n < max) n++;    /* include the NUL */
                h = fnv_bytes(h, p, n);
            }
            break;
        }
        case ZEUS_EFFECT_SEND:
        case ZEUS_EFFECT_RECV:
        case ZEUS_EFFECT_RAW_SET_PAYLOAD:
        case ZEUS_EFFECT_RAW_RECV:
            if (range_ok((int64_t)eff->buf, (int64_t)eff->len)) {
                h = fnv_bytes(h, vm->memory + eff->buf, (size_t)eff->len);
            }
            break;
        case ZEUS_EFFECT_RAW_SEND:
            if (eff->buf != 0 && eff->buf < ZEUS_MEMORY_SIZE) {
                const uint8_t *p = vm->memory + eff->buf;
                size_t max = ZEUS_MEMORY_SIZE - (size_t)eff->buf;
                size_t n = bounded_strlen(p, max);
                if (n < max) n++;
                h = fnv_bytes(h, p, n);
            }
            break;
        default:
            break;
    }
    return h;
}

/* ------------------------------------------------------------------ */
/* Key/value files                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    char key[32];
    char val[ZEUS_PATH_LEN];
} Pair;

/* Parse a key=value file. This format is written by this program and read by
 * a human decision, so it is parsed strictly: anything that could mean two
 * things (a cut-off line, a duplicate key, a value that does not fit) is
 * rejected instead of guessed at. */
static bool load_pairs(const char *path, Pair *pairs, size_t max, size_t *count) {
    *count = 0;
    FILE *f = fopen(path, "r");
    if (!f) return false;

    char line[ZEUS_PATH_LEN + 64];
    size_t n = 0;

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        /* No newline means the file was cut off mid-value or the line is
         * longer than any valid field. Accepting the prefix would turn a
         * truncated digest into a different digest. */
        if (len == 0 || line[len - 1] != '\n') { fclose(f); return false; }
        line[--len] = '\0';

        if (len == 0 || line[0] == '#') continue;

        char *eq = strchr(line, '=');
        if (!eq || eq == line) { fclose(f); return false; }
        if (n >= max) { fclose(f); return false; }

        size_t klen = (size_t)(eq - line);
        if (klen >= sizeof(pairs[n].key)) { fclose(f); return false; }
        memcpy(pairs[n].key, line, klen);
        pairs[n].key[klen] = '\0';

        /* Duplicate keys are ambiguous: another reader (or a human) can
         * disagree with this one about which value counts. Reject. */
        for (size_t i = 0; i < n; i++) {
            if (strcmp(pairs[i].key, pairs[n].key) == 0) {
                fclose(f);
                return false;
            }
        }

        int written = snprintf(pairs[n].val, sizeof(pairs[n].val), "%s", eq + 1);
        if (written < 0 || (size_t)written >= sizeof(pairs[n].val)) {
            fclose(f);
            return false;
        }
        n++;
    }

    bool ok = !ferror(f);
    fclose(f);
    *count = n;
    return ok;
}

static const char *pair_get(const Pair *pairs, size_t n, const char *key) {
    for (size_t i = 0; i < n; i++) {
        if (strcmp(pairs[i].key, key) == 0) return pairs[i].val;
    }
    return NULL;
}

/* Explicitly bounded copy: the compiler cannot prove a value read from a file
 * is short, and a silent truncation here would corrupt an audit field. */
static void copy_field(char *dst, size_t cap, const char *src) {
    size_t n = src ? strlen(src) : 0;
    if (n >= cap) n = cap - 1;
    if (n > 0) memcpy(dst, src, n);
    dst[n] = '\0';
}

static bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static bool parse_digest(const char *s, uint64_t *out) {
    if (!s || !*s) return false;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    /* strtoull accepts a leading sign or whitespace; a digest is bare hex. */
    if (!is_hex_digit(*s)) return false;
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 16);
    if (errno != 0 || !end || *end != '\0') return false;
    *out = (uint64_t)v;
    return true;
}

bool zeus_dossier_load(const char *path, ZeusDossier *out) {
    if (!path || !out) return false;

    Pair pairs[20];
    size_t n = 0;
    if (!load_pairs(path, pairs, 20, &n)) return false;

    const char *format = pair_get(pairs, n, "format");
    const char *kind = pair_get(pairs, n, "kind");
    const char *lineage = pair_get(pairs, n, "lineage");
    const char *seq = pair_get(pairs, n, "seq");
    const char *digest = pair_get(pairs, n, "digest");
    const char *effect = pair_get(pairs, n, "effect");

    memset(out, 0, sizeof(*out));
    if (!format || strcmp(format, "1") != 0) return false;
    if (!kind || strcmp(kind, "dossier") != 0) return false;
    if (!lineage || !zeus_valid_id(lineage)) return false;
    if (!seq || !parse_u64(seq, &out->seq)) return false;
    if (!digest || !parse_digest(digest, &out->digest)) return false;

    copy_field(out->lineage, sizeof(out->lineage), lineage);
    copy_field(out->effect, sizeof(out->effect), effect ? effect : "unknown");
    copy_field(out->snapshot, sizeof(out->snapshot),
               pair_get(pairs, n, "snapshot"));
    return true;
}

bool zeus_approval_load(const char *path, ZeusApproval *out) {
    if (!path || !out) return false;

    Pair pairs[12];
    size_t n = 0;
    if (!load_pairs(path, pairs, 12, &n)) return false;

    const char *format = pair_get(pairs, n, "format");
    const char *kind = pair_get(pairs, n, "kind");
    const char *lineage = pair_get(pairs, n, "lineage");
    const char *seq = pair_get(pairs, n, "seq");
    const char *digest = pair_get(pairs, n, "digest");

    memset(out, 0, sizeof(*out));
    if (!format || strcmp(format, "1") != 0) return false;
    if (!kind || strcmp(kind, "approval") != 0) return false;
    if (!lineage || !zeus_valid_id(lineage)) return false;
    if (!seq || !parse_u64(seq, &out->seq)) return false;
    if (!digest || !parse_digest(digest, &out->digest)) return false;

    copy_field(out->lineage, sizeof(out->lineage), lineage);
    return true;
}

bool zeus_snapshot_matches_dossier(const ZeusSnapshotMeta *m,
                                   const ZeusDossier *d,
                                   char *why, size_t why_len) {
    if (!m || !d) return false;

    /* An empty lineage means the snapshot is unbound. It cannot name an audit
     * chain, so it is refused below by the sequence check, which no dossier
     * can satisfy with seq 0. */
    if (m->lineage[0] != '\0' && strcmp(m->lineage, d->lineage) != 0) {
        set_why(why, why_len,
                "snapshot belongs to lineage %s but the dossier says %s; "
                "refusing to pair a machine with a different audit chain",
                m->lineage, d->lineage);
        return false;
    }
    if (m->seq != d->seq) {
        set_why(why, why_len,
                "snapshot was parked at seq %llu but the dossier says %llu; "
                "the journal was reset or replaced",
                (unsigned long long)m->seq, (unsigned long long)d->seq);
        return false;
    }
    return true;
}

bool zeus_approval_write(const ZeusDossier *d, const char *path) {
    if (!d || !path) return false;

    /* "wx": an approval is a new decision, never a silent overwrite. */
    FILE *f = fopen(path, "wx");
    if (!f) return false;

    fprintf(f, "format=1\n");
    fprintf(f, "kind=approval\n");
    fprintf(f, "lineage=%s\n", d->lineage);
    fprintf(f, "seq=%llu\n", (unsigned long long)d->seq);
    fprintf(f, "digest=0x%016llx\n", (unsigned long long)d->digest);
    fprintf(f, "effect=%s\n", d->effect);

    bool ok = !ferror(f);
    if (fclose(f) != 0) ok = false;
    return ok;
}

/* ------------------------------------------------------------------ */
/* Supervisor lifecycle                                                */
/* ------------------------------------------------------------------ */

void zeus_supervisor_init(ZeusSupervisor *s) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
    make_id(s->machine_id);
    make_id(s->lineage_id);
    zeus_policy_init(&s->policy, ZEUS_PROFILE_DENY_ALL);
}

bool zeus_supervisor_open(ZeusSupervisor *s, const ZeusPolicy *policy,
                          const char *journal_path, const char *lineage) {
    if (!s) return false;

    zeus_supervisor_init(s);
    if (policy) s->policy = *policy;

    if (lineage && *lineage) {
        if (!zeus_valid_id(lineage)) return false;
        snprintf(s->lineage_id, sizeof(s->lineage_id), "%s", lineage);
    }

    if (!journal_path) return true;

    /* Append, never truncate: the audit trail outlives the process. */
    int fd = open(journal_path, O_RDWR | O_CREAT | O_APPEND, 0666);
    if (fd < 0) return false;

    /* The lock is taken before the counter is scanned: two supervisors could
     * otherwise read the same last seq and mint the same number. A second
     * supervisor fails instead of waiting behind a machine that may run for a
     * long time. The lock lives as long as the FILE, i.e. until close. */
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        s->journal_busy = true;
        close(fd);
        return false;
    }

    ZeusJournalScan sc;
    char why[256];
    if (!zeus_journal_scan(journal_path, s->lineage_id, &sc,
                           why, sizeof(why))) {
        snprintf(s->journal_error, sizeof(s->journal_error), "%s", why);
        close(fd);
        return false;
    }
    s->seq = sc.max_seq;
    s->ambiguous = sc.ambiguous;
    s->ambiguous_seq = sc.ambiguous_seq;
    s->head_hash = sc.head_hash;
    s->chain_unverified = sc.chain_unverified;
    if (sc.chain_unverified) {
        fprintf(stderr,
                "warning: journal '%s' has legacy records without prev; "
                "chain unverified\n", journal_path);
    }

    s->journal = fdopen(fd, "a");
    if (!s->journal) {
        close(fd);
        return false;
    }

    /* Never append a `start` behind an unmatched intent: that record would
     * make the next retry look clean and replay an effect that may already
     * have happened. Refusing without writing keeps the evidence. */
    if (!s->ambiguous && !journal_start(s)) {
        fclose(s->journal);
        s->journal = NULL;
        return false;
    }
    return true;
}

void zeus_supervisor_close(ZeusSupervisor *s) {
    if (!s) return;
    if (s->journal) {
        fclose(s->journal);
        s->journal = NULL;
    }
}

void zeus_supervisor_set_paths(ZeusSupervisor *s, const char *snapshot_path) {
    if (!s) return;

    s->snapshot_path[0] = '\0';
    s->dossier_path[0] = '\0';

    copy_field(s->snapshot_path, sizeof(s->snapshot_path), snapshot_path);
    if (s->snapshot_path[0] != '\0') {
        size_t n = strlen(s->snapshot_path);
        if (n + 9 > sizeof(s->dossier_path)) n = sizeof(s->dossier_path) - 9;
        memcpy(s->dossier_path, s->snapshot_path, n);
        memcpy(s->dossier_path + n, ".dossier", 9);
    }
}

void zeus_supervisor_bind_approval(ZeusSupervisor *s, const ZeusApproval *a) {
    if (!s) return;
    s->have_approval = a != NULL;
    if (a) s->approval = *a;
    else memset(&s->approval, 0, sizeof(s->approval));
}

uint64_t zeus_supervisor_next_seq(const ZeusSupervisor *s) {
    return s ? s->seq + 1 : 0;
}

bool zeus_supervisor_deny(ZeusSupervisor *s, ZeusVM *vm, const char *reason) {
    if (!s || !vm || !vm->yielded) return false;

    ZeusEffect eff = vm->pending;
    uint64_t seq = zeus_supervisor_next_seq(s);
    uint64_t digest = zeus_effect_digest(vm, &eff);

    return deny_effect(s, vm, seq, &eff, digest,
                       reason ? reason : "operator_denied");
}

bool zeus_supervisor_write_dossier(const ZeusSupervisor *s, const ZeusVM *vm,
                                   const ZeusEffect *eff,
                                   const char *dossier_path) {
    if (!s || !vm || !eff || !dossier_path || !*dossier_path) return false;

    uint64_t digest = zeus_effect_digest(vm, eff);

    FILE *f = fopen(dossier_path, "w");
    if (!f) return false;

    fprintf(f, "format=1\n");
    fprintf(f, "kind=dossier\n");
    fprintf(f, "lineage=%s\n", s->lineage_id);
    fprintf(f, "seq=%llu\n", (unsigned long long)zeus_supervisor_next_seq(s));
    fprintf(f, "effect=%s\n", zeus_effect_name(eff->kind));
    fprintf(f, "fd=%lld\n", (long long)eff->fd);
    fprintf(f, "id=%lld\n", (long long)eff->id);
    fprintf(f, "value=%lld\n", (long long)eff->value);
    fprintf(f, "buf=%u\n", eff->buf);
    fprintf(f, "len=%u\n", eff->len);
    fprintf(f, "digest=0x%016llx\n", (unsigned long long)digest);
    fprintf(f, "snapshot=%s\n", s->snapshot_path);

    bool ok = !ferror(f);
    if (fclose(f) != 0) ok = false;
    return ok;
}

bool zeus_approval_binds(const ZeusApproval *a, const ZeusSupervisor *s,
                         const ZeusVM *vm, const ZeusEffect *eff) {
    if (!a || !s || !vm || !eff) return false;
    if (strcmp(a->lineage, s->lineage_id) != 0) return false;
    if (a->seq != zeus_supervisor_next_seq(s)) return false;
    return a->digest == zeus_effect_digest(vm, eff);
}

/* ------------------------------------------------------------------ */
/* The drive loop                                                      */
/* ------------------------------------------------------------------ */

ZeusDriveResult zeus_supervisor_drive(ZeusSupervisor *s, ZeusVM *vm,
                                      long max_steps,
                                      char *why, size_t why_len) {
    if (!s || !vm) {
        set_why(why, why_len, "invalid supervisor or machine");
        return ZEUS_DRIVE_FAILED;
    }

    if (s->ambiguous) {
        set_why(why, why_len,
                "journal ends in an uncommitted intent at seq %llu; "
                "refusing to replay an effect that may already have happened",
                (unsigned long long)s->ambiguous_seq);
        return ZEUS_DRIVE_FAILED;
    }

    if (!vm->started) zeus_vm_start(vm);

    long steps = 0;

    while (vm->running) {
        if (max_steps >= 0 && steps >= max_steps) {
            return ZEUS_DRIVE_STEP_LIMIT;
        }

        if (vm->yielded) {
            ZeusEffect eff = vm->pending;
            uint64_t seq = zeus_supervisor_next_seq(s);
            uint64_t digest = zeus_effect_digest(vm, &eff);

            if (zeus_policy_check(&s->policy, &eff) != ZEUS_POLICY_ALLOW) {
                const char *reason = zeus_policy_reason(&s->policy, &eff);
                if (!deny_effect(s, vm, seq, &eff, digest, reason)) {
                    set_why(why, why_len,
                            "policy denied %s (%s) but the denial could not be "
                            "journaled", zeus_effect_name(eff.kind), reason);
                    return ZEUS_DRIVE_DENY_UNRECORDED;
                }
                set_why(why, why_len, "policy denied %s (%s)",
                        zeus_effect_name(eff.kind), reason);
                return ZEUS_DRIVE_DENIED;
            }

            if (s->ask && zeus_policy_needs_approval(&s->policy, eff.kind)) {
                if (s->have_approval) {
                    if (!zeus_approval_binds(&s->approval, s, vm, &eff)) {
                        if (!deny_effect(s, vm, seq, &eff, digest,
                                         "approval_mismatch")) {
                            set_why(why, why_len,
                                    "approval does not match %s seq %llu but "
                                    "the denial could not be journaled",
                                    zeus_effect_name(eff.kind),
                                    (unsigned long long)seq);
                            return ZEUS_DRIVE_DENY_UNRECORDED;
                        }
                        set_why(why, why_len,
                                "approval does not match %s seq %llu digest 0x%016llx",
                                zeus_effect_name(eff.kind),
                                (unsigned long long)seq,
                                (unsigned long long)digest);
                        return ZEUS_DRIVE_DENIED;
                    }
                    /* An approval authorises exactly one effect. Reusing it
                     * for the next one would turn a single decision into a
                     * standing grant. */
                    zeus_supervisor_bind_approval(s, NULL);
                } else {
                    if (s->snapshot_path[0] == '\0' ||
                        s->dossier_path[0] == '\0') {
                        set_why(why, why_len,
                                "approval required but no snapshot/dossier path was set");
                        return ZEUS_DRIVE_FAILED;
                    }
                    /* The parked file carries the audit identity it belongs
                     * to, so a dossier cannot later be paired with a
                     * different machine or a snapshot from another chain. */
                    /* The park record comes first so the snapshot can name
                     * the journal head it was parked at. Restore accepts a
                     * snapshot whose head is in the chain, which keeps later
                     * resolve records from invalidating a parked machine. */
                    if (!journal_park(s, seq, &eff, digest)) {
                        set_why(why, why_len, "journal write failed");
                        return ZEUS_DRIVE_FAILED;
                    }
                    if (zeus_vm_snapshot_bound_ex(vm, s->snapshot_path,
                                                  s->lineage_id, seq,
                                                  s->head_hash) != ZEUS_OK) {
                        set_why(why, why_len, "cannot write snapshot '%s'",
                                s->snapshot_path);
                        return ZEUS_DRIVE_FAILED;
                    }
                    if (!zeus_supervisor_write_dossier(s, vm, &eff,
                                                       s->dossier_path)) {
                        set_why(why, why_len, "cannot write dossier '%s'",
                                s->dossier_path);
                        return ZEUS_DRIVE_FAILED;
                    }
                    set_why(why, why_len,
                            "%s seq %llu parked for approval (digest 0x%016llx)",
                            zeus_effect_name(eff.kind), (unsigned long long)seq,
                            (unsigned long long)digest);
                    return ZEUS_DRIVE_NEEDS_APPROVAL;
                }
            }

            /* Intent before delivery: if the process dies here, the next open
             * sees an intent with no commit and refuses to continue. */
            if (!journal_intent(s, seq, &eff, digest)) {
                set_why(why, why_len, "journal write failed");
                return ZEUS_DRIVE_FAILED;
            }

            int64_t result = -1;
            ZeusError err = zeus_vm_deliver_native(vm, &result);

            if (!journal_commit(s, seq, result, err)) {
                set_why(why, why_len, "journal write failed after delivery");
                return ZEUS_DRIVE_FAILED;
            }
            s->seq = seq;

            if (err != ZEUS_OK) {
                journal_halt(s, vm, "effect_failed");
                set_why(why, why_len, "effect %s failed",
                        zeus_effect_name(eff.kind));
                return ZEUS_DRIVE_FAILED;
            }
        } else {
            ZeusEffect eff;
            ZeusError err = zeus_vm_exec(vm, &eff);
            if (err != ZEUS_OK) {
                journal_halt(s, vm, "vm_error");
                set_why(why, why_len, "%s (pc=%u)", zeus_error_string(err), vm->pc);
                return ZEUS_DRIVE_FAILED;
            }
            steps++;
        }
    }

    journal_halt(s, vm, "halted");
    return ZEUS_DRIVE_HALTED;
}
