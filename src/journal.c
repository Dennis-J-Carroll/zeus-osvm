#define _POSIX_C_SOURCE 200809L

#include "journal.h"
#include "supervisor.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define FNV_OFFSET UINT64_C(1469598103934665603)
#define FNV_PRIME  UINT64_C(1099511628211)

uint64_t zeus_journal_hash(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    uint64_t h = FNV_OFFSET;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= FNV_PRIME;
    }
    return h;
}

static void jset(char *why, size_t cap, const char *fmt, ...) {
    if (!why || cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, cap, fmt, ap);
    va_end(ap);
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_hex64(const char *s, uint64_t *out) {
    if (!s || strlen(s) != 16) return false;
    uint64_t v = 0;
    for (int i = 0; i < 16; i++) {
        int h = hex_value(s[i]);
        if (h < 0) return false;
        v = (v << 4) | (uint64_t)h;
    }
    *out = v;
    return true;
}

bool zeus_json_escape(const char *src, char *dst, size_t cap) {
    if (!src || !dst || cap == 0) return false;
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        const char *esc = NULL;
        char ubuf[8];
        switch (*p) {
            case '"':  esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\b': esc = "\\b"; break;
            case '\f': esc = "\\f"; break;
            case '\n': esc = "\\n"; break;
            case '\r': esc = "\\r"; break;
            case '\t': esc = "\\t"; break;
            default:
                if (*p < 0x20) {
                    snprintf(ubuf, sizeof(ubuf), "\\u%04x", (unsigned)*p);
                    esc = ubuf;
                }
                break;
        }
        const char *text = esc ? esc : (const char *)p;
        size_t n = esc ? strlen(esc) : 1;
        if (o + n + 1 > cap) return false;
        memcpy(dst + o, text, n);
        o += n;
    }
    dst[o] = '\0';
    return true;
}

/* ------------------------------------------------------------------ */
/* Record parsing                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    char event[16];
    char lineage[ZEUS_JOURNAL_ID_LEN];
    char machine[ZEUS_JOURNAL_ID_LEN];
    char outcome[16];
    uint64_t seq;
    uint64_t pending;
    uint64_t prev;
    bool has_event, has_lineage, has_machine, has_seq, has_pending;
    bool has_prev, has_outcome;
} JRecord;

enum {
    K_EVENT = 1u << 0, K_LINEAGE = 1u << 1, K_MACHINE = 1u << 2,
    K_SEQ = 1u << 3, K_PENDING = 1u << 4, K_PREV = 1u << 5,
    K_OUTCOME = 1u << 6, K_REASON = 1u << 7, K_EFFECT = 1u << 8,
    K_DIGEST = 1u << 9, K_RESULT = 1u << 10, K_ERROR = 1u << 11,
    K_PC = 1u << 12, K_FD = 1u << 13, K_ID = 1u << 14, K_VALUE = 1u << 15,
    K_BUF = 1u << 16, K_LEN = 1u << 17, K_RAW = 1u << 18
};

static bool string_key(const char *k) {
    return strcmp(k, "event") == 0 || strcmp(k, "lineage") == 0 ||
           strcmp(k, "machine") == 0 || strcmp(k, "prev") == 0 ||
           strcmp(k, "outcome") == 0 || strcmp(k, "reason") == 0 ||
           strcmp(k, "effect") == 0 || strcmp(k, "digest") == 0;
}

static bool number_key(const char *k) {
    return strcmp(k, "seq") == 0 || strcmp(k, "pending") == 0 ||
           strcmp(k, "result") == 0 || strcmp(k, "error") == 0 ||
           strcmp(k, "pc") == 0 || strcmp(k, "fd") == 0 ||
           strcmp(k, "id") == 0 || strcmp(k, "value") == 0 ||
           strcmp(k, "buf") == 0 || strcmp(k, "len") == 0 ||
           strcmp(k, "raw") == 0;
}

typedef struct {
    const char *p;
    const char *end;
} JP;

static void jp_ws(JP *j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t')) j->p++;
}

static bool jp_char(JP *j, char c) {
    if (j->p < j->end && *j->p == c) { j->p++; return true; }
    return false;
}

static bool jp_string(JP *j, char *out, size_t cap) {
    if (!jp_char(j, '"')) return false;
    size_t o = 0;
    while (j->p < j->end) {
        unsigned char c = (unsigned char)*j->p++;
        if (c == '"') {
            if (o >= cap) return false;
            out[o] = '\0';
            return true;
        }
        if (c < 0x20) return false;
        if (c == '\\') {
            if (j->p >= j->end) return false;
            char e = *j->p++;
            switch (e) {
                case '"':  c = '"'; break;
                case '\\': c = '\\'; break;
                case '/':  c = '/'; break;
                case 'b':  c = '\b'; break;
                case 'f':  c = '\f'; break;
                case 'n':  c = '\n'; break;
                case 'r':  c = '\r'; break;
                case 't':  c = '\t'; break;
                case 'u': {
                    if (j->end - j->p < 4) return false;
                    unsigned v = 0;
                    for (int i = 0; i < 4; i++) {
                        int h = hex_value(j->p[i]);
                        if (h < 0) return false;
                        v = (v << 4) | (unsigned)h;
                    }
                    j->p += 4;
                    if (v < 0x80) {
                        c = (unsigned char)v;
                    } else {
                        if (o + 4 >= cap) return false;
                        if (v < 0x800) {
                            out[o++] = (char)(0xC0 | (v >> 6));
                            out[o++] = (char)(0x80 | (v & 0x3F));
                        } else {
                            out[o++] = (char)(0xE0 | (v >> 12));
                            out[o++] = (char)(0x80 | ((v >> 6) & 0x3F));
                            out[o++] = (char)(0x80 | (v & 0x3F));
                        }
                        continue;
                    }
                    break;
                }
                default:
                    return false;
            }
        }
        if (o + 1 >= cap) return false;
        out[o++] = (char)c;
    }
    return false;
}

static bool jp_number(JP *j, uint64_t *out, bool *negative) {
    bool neg = false;
    if (j->p < j->end && *j->p == '-') { neg = true; j->p++; }
    if (j->p >= j->end || *j->p < '0' || *j->p > '9') return false;
    uint64_t v = 0;
    bool overflow = false;
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
        unsigned d = (unsigned)(*j->p++ - '0');
        if (v > (UINT64_MAX - d) / 10) overflow = true;
        else v = v * 10 + d;
    }
    /* JSON floats and exponents are not part of the journal format. */
    if (j->p < j->end && (*j->p == '.' || *j->p == 'e' || *j->p == 'E')) {
        return false;
    }
    if (overflow) return false;
    if (out) *out = v;
    if (negative) *negative = neg;
    return true;
}

static bool copy_id(char *dst, const char *src, const char *key,
                    char *why, size_t wl) {
    if (!zeus_valid_id(src)) {
        jset(why, wl, "field '%s' is not 32 hex characters", key);
        return false;
    }
    memcpy(dst, src, 32);
    dst[32] = '\0';
    return true;
}

static bool apply_string(JRecord *r, const char *key, const char *val,
                         uint32_t *seen, char *why, size_t wl) {
    uint32_t bit = 0;
    if (strcmp(key, "event") == 0) bit = K_EVENT;
    else if (strcmp(key, "lineage") == 0) bit = K_LINEAGE;
    else if (strcmp(key, "machine") == 0) bit = K_MACHINE;
    else if (strcmp(key, "prev") == 0) bit = K_PREV;
    else if (strcmp(key, "outcome") == 0) bit = K_OUTCOME;
    else if (strcmp(key, "reason") == 0) bit = K_REASON;
    else if (strcmp(key, "effect") == 0) bit = K_EFFECT;
    else if (strcmp(key, "digest") == 0) bit = K_DIGEST;
    else if (number_key(key)) {
        jset(why, wl, "field '%s' must be a number", key);
        return false;
    } else {
        return true;      /* unknown scalar field: syntax already checked */
    }

    if (*seen & bit) {
        jset(why, wl, "duplicate field '%s'", key);
        return false;
    }
    *seen |= bit;

    if (bit == K_EVENT) {
        size_t n = strlen(val);
        if (n >= sizeof(r->event)) {
            jset(why, wl, "event name is too long");
            return false;
        }
        memcpy(r->event, val, n + 1);
        r->has_event = true;
    } else if (bit == K_LINEAGE) {
        if (!copy_id(r->lineage, val, key, why, wl)) return false;
        r->has_lineage = true;
    } else if (bit == K_MACHINE) {
        if (!copy_id(r->machine, val, key, why, wl)) return false;
        r->has_machine = true;
    } else if (bit == K_PREV) {
        if (!parse_hex64(val, &r->prev)) {
            jset(why, wl, "field 'prev' is not 16 hex characters");
            return false;
        }
        r->has_prev = true;
    } else if (bit == K_OUTCOME) {
        size_t n = strlen(val);
        if (n >= sizeof(r->outcome)) {
            jset(why, wl, "outcome is too long");
            return false;
        }
        memcpy(r->outcome, val, n + 1);
        r->has_outcome = true;
    }
    /* reason/effect/digest are validated as strings and otherwise unused. */
    return true;
}

static bool apply_number(JRecord *r, const char *key, uint64_t val,
                         bool negative, uint32_t *seen,
                         char *why, size_t wl) {
    uint32_t bit = 0;
    if (strcmp(key, "seq") == 0) bit = K_SEQ;
    else if (strcmp(key, "pending") == 0) bit = K_PENDING;
    else if (strcmp(key, "result") == 0) bit = K_RESULT;
    else if (strcmp(key, "error") == 0) bit = K_ERROR;
    else if (strcmp(key, "pc") == 0) bit = K_PC;
    else if (strcmp(key, "fd") == 0) bit = K_FD;
    else if (strcmp(key, "id") == 0) bit = K_ID;
    else if (strcmp(key, "value") == 0) bit = K_VALUE;
    else if (strcmp(key, "buf") == 0) bit = K_BUF;
    else if (strcmp(key, "len") == 0) bit = K_LEN;
    else if (strcmp(key, "raw") == 0) bit = K_RAW;
    else if (string_key(key)) {
        jset(why, wl, "field '%s' must be a string", key);
        return false;
    } else {
        return true;
    }

    if (*seen & bit) {
        jset(why, wl, "duplicate field '%s'", key);
        return false;
    }
    *seen |= bit;

    if ((bit == K_SEQ || bit == K_PENDING) && negative) {
        jset(why, wl, "field '%s' cannot be negative", key);
        return false;
    }
    if (bit == K_SEQ) {
        r->seq = val;
        r->has_seq = true;
    } else if (bit == K_PENDING) {
        r->pending = val;
        r->has_pending = true;
    }
    return true;
}

static bool known_event(const char *e) {
    return strcmp(e, "start") == 0 || strcmp(e, "intent") == 0 ||
           strcmp(e, "commit") == 0 || strcmp(e, "park") == 0 ||
           strcmp(e, "deny") == 0 || strcmp(e, "halt") == 0 ||
           strcmp(e, "resolve") == 0;
}

static bool parse_record(const char *line, size_t len, JRecord *r,
                         char *why, size_t wl) {
    memset(r, 0, sizeof(*r));
    uint32_t seen = 0;
    JP j = { line, line + len };

    jp_ws(&j);
    if (!jp_char(&j, '{')) {
        jset(why, wl, "record is not a JSON object");
        return false;
    }
    jp_ws(&j);

    bool first = true;
    while (j.p < j.end && *j.p != '}') {
        if (!first) {
            if (!jp_char(&j, ',')) {
                jset(why, wl, "expected ',' between fields");
                return false;
            }
            jp_ws(&j);
        }
        first = false;

        char key[64];
        if (!jp_string(&j, key, sizeof(key))) {
            jset(why, wl, "malformed field name");
            return false;
        }
        jp_ws(&j);
        if (!jp_char(&j, ':')) {
            jset(why, wl, "expected ':' after field '%s'", key);
            return false;
        }
        jp_ws(&j);

        if (j.p < j.end && *j.p == '"') {
            char val[ZEUS_JOURNAL_LINE_MAX];
            if (!jp_string(&j, val, sizeof(val))) {
                jset(why, wl, "malformed string value for '%s'", key);
                return false;
            }
            if (!apply_string(r, key, val, &seen, why, wl)) return false;
        } else if (j.p < j.end &&
                   (*j.p == 't' || *j.p == 'f' || *j.p == 'n')) {
            const char *tok = NULL;
            size_t n = 0;
            if (j.end - j.p >= 4 && memcmp(j.p, "true", 4) == 0) {
                tok = "true"; n = 4;
            } else if (j.end - j.p >= 5 && memcmp(j.p, "false", 5) == 0) {
                tok = "false"; n = 5;
            } else if (j.end - j.p >= 4 && memcmp(j.p, "null", 4) == 0) {
                tok = "null"; n = 4;
            }
            if (!tok) {
                jset(why, wl, "malformed literal for '%s'", key);
                return false;
            }
            if (string_key(key) || number_key(key)) {
                jset(why, wl, "field '%s' has the wrong type", key);
                return false;
            }
            j.p += n;
        } else {
            uint64_t val = 0;
            bool negative = false;
            if (!jp_number(&j, &val, &negative)) {
                jset(why, wl, "malformed number for '%s'", key);
                return false;
            }
            if (!apply_number(r, key, val, negative, &seen, why, wl)) {
                return false;
            }
        }
        jp_ws(&j);
    }

    if (!jp_char(&j, '}')) {
        jset(why, wl, "record is not closed");
        return false;
    }
    jp_ws(&j);
    if (j.p != j.end) {
        jset(why, wl, "trailing bytes after the record");
        return false;
    }

    if (!r->has_event || !known_event(r->event)) {
        jset(why, wl, "missing or unknown event");
        return false;
    }
    if (!r->has_lineage) {
        jset(why, wl, "missing lineage");
        return false;
    }
    if (!r->has_seq) {
        jset(why, wl, "missing seq");
        return false;
    }
    if (r->has_machine && !zeus_valid_id(r->machine)) {
        jset(why, wl, "machine is not 32 hex characters");
        return false;
    }
    if (strcmp(r->event, "start") == 0 && r->seq != 0) {
        jset(why, wl, "start record has seq %llu", (unsigned long long)r->seq);
        return false;
    }
    if (strcmp(r->event, "resolve") == 0) {
        if (!r->has_outcome ||
            (strcmp(r->outcome, "committed") != 0 &&
             strcmp(r->outcome, "aborted") != 0)) {
            jset(why, wl, "resolve record has an invalid outcome");
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Sequence accounting and the chain walk                              */
/* ------------------------------------------------------------------ */

typedef struct {
    char lineage[ZEUS_JOURNAL_ID_LEN];
    uint64_t max_seq;
    uint64_t open_seq;
    char open_machine[ZEUS_JOURNAL_ID_LEN];
} LState;

typedef struct {
    LState *v;
    size_t n, cap;
} LTable;

static LState *state_get(LTable *t, const char *lineage) {
    for (size_t i = 0; i < t->n; i++) {
        if (strcmp(t->v[i].lineage, lineage) == 0) return &t->v[i];
    }
    if (t->n == t->cap) {
        size_t nc = t->cap ? t->cap * 2 : 8;
        LState *nv = (LState *)realloc(t->v, nc * sizeof(*nv));
        if (!nv) return NULL;
        t->v = nv;
        t->cap = nc;
    }
    LState *st = &t->v[t->n++];
    memset(st, 0, sizeof(*st));
    memcpy(st->lineage, lineage, 32);
    st->lineage[32] = '\0';
    return st;
}

static LState *state_find(LTable *t, const char *lineage) {
    for (size_t i = 0; i < t->n; i++) {
        if (strcmp(t->v[i].lineage, lineage) == 0) return &t->v[i];
    }
    return NULL;
}

static bool apply_state(uint64_t lineno, const JRecord *r, LTable *t,
                        char *why, size_t wl) {
    if (strcmp(r->event, "start") == 0) return true;

    LState *st = state_get(t, r->lineage);
    if (!st) {
        jset(why, wl, "out of memory while tracking lineages");
        return false;
    }

    if (strcmp(r->event, "intent") == 0) {
        if (st->open_seq != 0) {
            jset(why, wl, "line %llu: intent seq %llu while seq %llu is still open",
                 (unsigned long long)lineno, (unsigned long long)r->seq,
                 (unsigned long long)st->open_seq);
            return false;
        }
        if (r->seq != st->max_seq + 1) {
            jset(why, wl,
                 "line %llu: intent seq %llu is a duplicate or regressed seq "
                 "(expected %llu)", (unsigned long long)lineno,
                 (unsigned long long)r->seq,
                 (unsigned long long)(st->max_seq + 1));
            return false;
        }
        st->open_seq = r->seq;
        if (r->has_machine) {
            memcpy(st->open_machine, r->machine, sizeof(st->open_machine));
        } else {
            st->open_machine[0] = '\0';
        }
    } else if (strcmp(r->event, "commit") == 0) {
        if (st->open_seq != r->seq) {
            jset(why, wl, "line %llu: commit seq %llu has no matching intent",
                 (unsigned long long)lineno, (unsigned long long)r->seq);
            return false;
        }
        st->open_seq = 0;
        if (r->seq > st->max_seq) st->max_seq = r->seq;
    } else if (strcmp(r->event, "deny") == 0) {
        if (st->open_seq != 0) {
            jset(why, wl, "line %llu: deny seq %llu while seq %llu is still open",
                 (unsigned long long)lineno, (unsigned long long)r->seq,
                 (unsigned long long)st->open_seq);
            return false;
        }
        if (r->seq != st->max_seq + 1) {
            jset(why, wl,
                 "line %llu: deny seq %llu is a duplicate or regressed seq "
                 "(expected %llu)", (unsigned long long)lineno,
                 (unsigned long long)r->seq,
                 (unsigned long long)(st->max_seq + 1));
            return false;
        }
        if (r->seq > st->max_seq) st->max_seq = r->seq;
    } else if (strcmp(r->event, "resolve") == 0) {
        if (st->open_seq != r->seq) {
            jset(why, wl, "line %llu: resolve seq %llu has no open intent",
                 (unsigned long long)lineno, (unsigned long long)r->seq);
            return false;
        }
        st->open_seq = 0;
        if (r->seq > st->max_seq) st->max_seq = r->seq;
    } else if (strcmp(r->event, "park") == 0) {
        if (st->open_seq != 0) {
            jset(why, wl, "line %llu: park while seq %llu is still open",
                 (unsigned long long)lineno,
                 (unsigned long long)st->open_seq);
            return false;
        }
        if (r->seq != st->max_seq) {
            jset(why, wl, "line %llu: park names seq %llu, expected %llu",
                 (unsigned long long)lineno, (unsigned long long)r->seq,
                 (unsigned long long)st->max_seq);
            return false;
        }
        if (!r->has_pending || r->pending != st->max_seq + 1) {
            jset(why, wl,
                 "line %llu: park does not name the next pending seq %llu",
                 (unsigned long long)lineno,
                 (unsigned long long)(st->max_seq + 1));
            return false;
        }
    } else if (strcmp(r->event, "halt") == 0) {
        if (st->open_seq != 0) {
            jset(why, wl, "line %llu: halt while seq %llu is still open",
                 (unsigned long long)lineno,
                 (unsigned long long)st->open_seq);
            return false;
        }
        if (r->seq != st->max_seq) {
            jset(why, wl, "line %llu: halt names seq %llu, expected %llu",
                 (unsigned long long)lineno, (unsigned long long)r->seq,
                 (unsigned long long)st->max_seq);
            return false;
        }
    }
    return true;
}

typedef struct {
    uint64_t records;
    uint64_t error_line;
    uint64_t first_legacy_line;
    bool legacy;
    uint64_t head_hash;
    bool head_found;
} JScan;

static bool scan_file(const char *path, const char *lineage,
                      bool has_wanted, uint64_t wanted,
                      LTable *table, JScan *out,
                      char *why, size_t wl) {
    memset(out, 0, sizeof(*out));
    FILE *f = fopen(path, "r");
    if (!f) {
        jset(why, wl, "cannot read journal '%s'", path);
        return false;
    }

    char *line = NULL;
    size_t cap = 0;
    ssize_t n = 0;
    uint64_t lineno = 0;
    uint64_t running = 0;
    bool ok = true;

    while ((n = getline(&line, &cap, f)) >= 0) {
        lineno++;
        if ((size_t)n > ZEUS_JOURNAL_LINE_MAX) {
            out->error_line = lineno;
            jset(why, wl, "journal record too long at line %llu",
                 (unsigned long long)lineno);
            ok = false;
            break;
        }
        if (n == 0 || line[n - 1] != '\n') {
            out->error_line = lineno;
            jset(why, wl, "truncated record at line %llu (no newline)",
                 (unsigned long long)lineno);
            ok = false;
            break;
        }
        size_t len = (size_t)n - 1;

        JRecord r;
        char pwhy[192];
        if (!parse_record(line, len, &r, pwhy, sizeof(pwhy))) {
            out->error_line = lineno;
            jset(why, wl, "line %llu: %s",
                 (unsigned long long)lineno, pwhy);
            ok = false;
            break;
        }

        if (r.has_prev) {
            if (r.prev != running) {
                out->error_line = lineno;
                jset(why, wl,
                     "line %llu: prev 0x%016llx does not match previous "
                     "record 0x%016llx (chain break)",
                     (unsigned long long)lineno,
                     (unsigned long long)r.prev,
                     (unsigned long long)running);
                ok = false;
                break;
            }
        } else {
            out->legacy = true;
            if (out->first_legacy_line == 0) out->first_legacy_line = lineno;
        }

        running = zeus_journal_hash(line, len);
        out->records++;
        if (has_wanted && running == wanted) out->head_found = true;

        if (!lineage || strcmp(r.lineage, lineage) == 0) {
            if (!apply_state(lineno, &r, table, why, wl)) {
                out->error_line = lineno;
                ok = false;
                break;
            }
        }
    }

    if (ok && ferror(f)) {
        out->error_line = lineno;
        jset(why, wl, "error reading journal '%s'", path);
        ok = false;
    }
    out->head_hash = running;
    free(line);
    fclose(f);
    return ok;
}

bool zeus_journal_scan(const char *path, const char *lineage,
                       ZeusJournalScan *out, char *why, size_t wl) {
    if (!path || !out) return false;
    memset(out, 0, sizeof(*out));
    if (lineage && !zeus_valid_id(lineage)) {
        jset(why, wl, "invalid lineage id");
        return false;
    }

    LTable table = {0};
    JScan sc;
    bool ok = scan_file(path, lineage, false, 0, &table, &sc, why, wl);
    if (ok) {
        out->head_hash = sc.head_hash;
        out->chain_unverified = sc.legacy;
        out->max_seq = 0;
        out->ambiguous = false;
        out->ambiguous_seq = 0;
        if (lineage) {
            LState *st = state_find(&table, lineage);
            if (st) {
                out->max_seq = st->max_seq;
                out->ambiguous = st->open_seq != 0;
                out->ambiguous_seq = st->open_seq;
            }
        }
    }
    free(table.v);
    return ok;
}

bool zeus_journal_contains_head(const char *path, uint64_t head, bool *found,
                                char *why, size_t wl) {
    if (found) *found = false;
    if (!path || !found) return false;
    if (head == 0) {
        *found = true;      /* unbound snapshot */
        return true;
    }

    LTable table = {0};
    JScan sc;
    bool ok = scan_file(path, NULL, true, head, &table, &sc, why, wl);
    free(table.v);
    if (ok) *found = sc.head_found;
    return ok;
}

bool zeus_journal_verify(const char *path, const char *lineage,
                         bool allow_legacy, ZeusJournalVerify *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!path) {
        snprintf(out->reason, sizeof(out->reason), "no journal path");
        return false;
    }
    if (lineage && !zeus_valid_id(lineage)) {
        snprintf(out->reason, sizeof(out->reason), "invalid lineage id");
        return false;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        snprintf(out->reason, sizeof(out->reason),
                 "cannot open journal '%s'", path);
        return false;
    }
    if (flock(fd, LOCK_SH | LOCK_NB) != 0) {
        snprintf(out->reason, sizeof(out->reason),
                 "journal '%s' is held by another process", path);
        close(fd);
        return false;
    }

    LTable table = {0};
    JScan sc;
    bool ok = scan_file(path, lineage, false, 0, &table, &sc,
                        out->reason, sizeof(out->reason));
    free(table.v);
    close(fd);

    out->records = sc.records;
    out->legacy = sc.legacy;
    if (!ok) {
        out->line = sc.error_line;
        return false;
    }
    if (sc.legacy && !allow_legacy) {
        out->line = sc.first_legacy_line;
        snprintf(out->reason, sizeof(out->reason),
                 "line %llu: record has no prev (legacy chain); pass "
                 "--allow-legacy to accept",
                 (unsigned long long)sc.first_legacy_line);
        return false;
    }
    return true;
}

bool zeus_journal_resolve(const char *path, uint64_t seq,
                          const char *outcome, const char *reason,
                          char *why, size_t wl) {
    if (!path || !outcome) {
        jset(why, wl, "resolve needs a journal and an outcome");
        return false;
    }
    if (strcmp(outcome, "committed") != 0 &&
        strcmp(outcome, "aborted") != 0) {
        jset(why, wl, "outcome must be 'committed' or 'aborted'");
        return false;
    }
    if (!reason) reason = "";

    int fd = open(path, O_RDWR | O_APPEND);
    if (fd < 0) {
        jset(why, wl, "cannot open journal '%s'", path);
        return false;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        jset(why, wl, "journal '%s' is held by another supervisor", path);
        close(fd);
        return false;
    }

    LTable table = {0};
    JScan sc;
    bool ok = scan_file(path, NULL, false, 0, &table, &sc, why, wl);
    size_t matches = 0;
    LState *hit = NULL;
    if (ok) {
        for (size_t i = 0; i < table.n; i++) {
            if (table.v[i].open_seq == seq) {
                matches++;
                hit = &table.v[i];
            }
        }
    }

    if (!ok) {
        free(table.v);
        close(fd);
        return false;
    }
    if (matches == 0) {
        jset(why, wl, "no open intent at seq %llu", (unsigned long long)seq);
        free(table.v);
        close(fd);
        return false;
    }
    if (matches > 1) {
        jset(why, wl,
             "more than one lineage has an open intent at seq %llu",
             (unsigned long long)seq);
        free(table.v);
        close(fd);
        return false;
    }

    char escaped[1024];
    if (!zeus_json_escape(reason, escaped, sizeof(escaped))) {
        jset(why, wl, "resolve reason is too long to journal");
        free(table.v);
        close(fd);
        return false;
    }

    const char *machine = hit->open_machine[0] ? hit->open_machine
                          : "00000000000000000000000000000000";
    char record[ZEUS_JOURNAL_LINE_MAX];
    int n = snprintf(record, sizeof(record),
                     "{\"prev\":\"%016llx\",\"seq\":%llu,"
                     "\"event\":\"resolve\",\"machine\":\"%s\","
                     "\"lineage\":\"%s\",\"outcome\":\"%s\","
                     "\"reason\":\"%s\"}",
                     (unsigned long long)sc.head_hash,
                     (unsigned long long)seq, machine, hit->lineage,
                     outcome, escaped);
    if (n < 0 || (size_t)n >= sizeof(record)) {
        jset(why, wl, "resolve record is too long to journal");
        free(table.v);
        close(fd);
        return false;
    }

    ok = zeus_journal_append_fd(fd, record, (size_t)n);
    if (!ok) jset(why, wl, "cannot append the resolve record");
    free(table.v);
    close(fd);
    return ok;
}

bool zeus_journal_append_fd(int fd, const char *record, size_t len) {
    if (fd < 0 || !record || len == 0 || len + 1 > ZEUS_JOURNAL_LINE_MAX) {
        return false;
    }

    char buf[ZEUS_JOURNAL_LINE_MAX + 1];
    memcpy(buf, record, len);
    buf[len] = '\n';

    struct stat st;
    if (fstat(fd, &st) != 0) return false;
    off_t before = st.st_size;

    ssize_t wrote = write(fd, buf, len + 1);
    if (wrote != (ssize_t)(len + 1) || fsync(fd) != 0) {
        int rolled_back = ftruncate(fd, before);
        (void)rolled_back;
        return false;
    }
    return true;
}
