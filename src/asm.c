#define _POSIX_C_SOURCE 200809L
#include "asm.h"
#include "isa.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#define MAX_SYMBOLS 1024
#define MAX_FIXUPS  1024
#define MAX_LINE    1024

typedef enum { SEC_CODE, SEC_DATA } Section;

typedef struct {
    char name[64];
    Section section;
    uint32_t offset;
} Symbol;

typedef struct {
    Section section;
    uint32_t offset;
    char name[64];
    int line;
} Fixup;

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
} ByteBuf;

static void buf_init(ByteBuf *b) {
    b->buf = NULL;
    b->len = 0;
    b->cap = 0;
}

static void buf_free(ByteBuf *b) {
    free(b->buf);
    b->buf = NULL;
    b->len = 0;
    b->cap = 0;
}

static int buf_append(ByteBuf *b, const uint8_t *data, size_t n) {
    if (b->len + n > b->cap) {
        size_t new_cap = b->cap ? b->cap * 2 : 256;
        while (new_cap < b->len + n) new_cap *= 2;
        uint8_t *p = (uint8_t *)realloc(b->buf, new_cap);
        if (!p) return -1;
        b->buf = p;
        b->cap = new_cap;
    }
    memcpy(b->buf + b->len, data, n);
    b->len += n;
    return 0;
}

static void buf_write_i64(ByteBuf *b, int64_t v) {
    uint8_t p[8];
    uint64_t u = (uint64_t)v;
    p[0] = (uint8_t)(u & 0xFF);
    p[1] = (uint8_t)((u >> 8) & 0xFF);
    p[2] = (uint8_t)((u >> 16) & 0xFF);
    p[3] = (uint8_t)((u >> 24) & 0xFF);
    p[4] = (uint8_t)((u >> 32) & 0xFF);
    p[5] = (uint8_t)((u >> 40) & 0xFF);
    p[6] = (uint8_t)((u >> 48) & 0xFF);
    p[7] = (uint8_t)((u >> 56) & 0xFF);
    buf_append(b, p, 8);
}

typedef struct {
    Section section;
    ByteBuf code;
    ByteBuf data;

    Symbol symbols[MAX_SYMBOLS];
    int sym_count;

    Fixup fixups[MAX_FIXUPS];
    int fix_count;

    char err[512];
} Assembler;

static void asm_init(Assembler *a) {
    memset(a, 0, sizeof(*a));
    a->section = SEC_CODE;
    buf_init(&a->code);
    buf_init(&a->data);
}

static void asm_free(Assembler *a) {
    buf_free(&a->code);
    buf_free(&a->data);
}

static ByteBuf *current_buf(Assembler *a) {
    return (a->section == SEC_CODE) ? &a->code : &a->data;
}

static Symbol *find_symbol(Assembler *a, const char *name) {
    for (int i = 0; i < a->sym_count; i++) {
        if (strcmp(a->symbols[i].name, name) == 0) {
            return &a->symbols[i];
        }
    }
    return NULL;
}

static int add_symbol(Assembler *a, const char *name, Section section, uint32_t offset, int line) {
    if (a->sym_count >= MAX_SYMBOLS) {
        snprintf(a->err, sizeof(a->err), "line %d: too many symbols", line);
        return -1;
    }
    if (find_symbol(a, name)) {
        snprintf(a->err, sizeof(a->err), "line %d: duplicate symbol '%s'", line, name);
        return -1;
    }
    Symbol *s = &a->symbols[a->sym_count++];
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->section = section;
    s->offset = offset;
    return 0;
}

static int add_fixup(Assembler *a, Section section, uint32_t offset, const char *name, int line) {
    if (a->fix_count >= MAX_FIXUPS) {
        snprintf(a->err, sizeof(a->err), "line %d: too many fixups", line);
        return -1;
    }
    Fixup *f = &a->fixups[a->fix_count++];
    f->section = section;
    f->offset = offset;
    snprintf(f->name, sizeof(f->name), "%s", name);
    f->line = line;
    return 0;
}

static int parse_number(const char *tok, int64_t *out) {
    char *end;
    errno = 0;
    long long v = strtoll(tok, &end, 0);
    if (errno != 0 || *end != '\0') return -1;
    *out = (int64_t)v;
    return 0;
}

static int parse_string(const char *p, char *out, size_t out_len, size_t *out_used) {
    if (*p != '"') return -1;
    p++;
    size_t i = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n': if (i < out_len) out[i++] = '\n'; break;
                case 't': if (i < out_len) out[i++] = '\t'; break;
                case 'r': if (i < out_len) out[i++] = '\r'; break;
                case '0': if (i < out_len) out[i++] = '\0'; break;
                case '\\': if (i < out_len) out[i++] = '\\'; break;
                case '"': if (i < out_len) out[i++] = '"'; break;
                default: if (i < out_len) out[i++] = *p; break;
            }
            p++;
        } else {
            if (i < out_len) out[i++] = *p;
            p++;
        }
    }
    if (*p != '"') return -1;
    *out_used = i;
    return 0;
}

static int is_label_char(char c) {
    return isalnum((unsigned char)c) || c == '_' || c == '.';
}

static int emit_operand(Assembler *a, const char *tok, int line) {
    ByteBuf *buf = current_buf(a);
    int64_t num;
    if (parse_number(tok, &num) == 0) {
        buf_write_i64(buf, num);
        return 0;
    }
    /* Treat as label. */
    uint32_t off = (uint32_t)buf->len;
    buf_write_i64(buf, 0); /* placeholder */
    return add_fixup(a, a->section, off, tok, line);
}

static int process_line(Assembler *a, char *line, int line_no) {
    char *p = line;
    /* Skip leading whitespace. */
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p || *p == ';') return 0;

    /* Check for label at start of line. */
    char *label_end = p;
    while (is_label_char(*label_end)) label_end++;
    if (*label_end == ':' && label_end > p) {
        char name[64];
        size_t len = (size_t)(label_end - p);
        if (len >= sizeof(name)) len = sizeof(name) - 1;
        memcpy(name, p, len);
        name[len] = '\0';
        ByteBuf *buf = current_buf(a);
        if (add_symbol(a, name, a->section, (uint32_t)buf->len, line_no) != 0) return -1;
        p = label_end + 1;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p || *p == ';') return 0;
    }

    /* Extract mnemonic/directive token. */
    char *start = p;
    while (*p && !isspace((unsigned char)*p) && *p != ';') p++;
    size_t tok_len = (size_t)(p - start);
    if (tok_len == 0) return 0;

    char mnem[64];
    if (tok_len >= sizeof(mnem)) tok_len = sizeof(mnem) - 1;
    memcpy(mnem, start, tok_len);
    mnem[tok_len] = '\0';

    /* Skip whitespace to operand. */
    while (*p && isspace((unsigned char)*p)) p++;

    /* Directives. */
    if (mnem[0] == '.') {
        if (strcmp(mnem, ".code") == 0) {
            a->section = SEC_CODE;
            return 0;
        }
        if (strcmp(mnem, ".data") == 0) {
            a->section = SEC_DATA;
            return 0;
        }
        if (strcmp(mnem, ".byte") == 0) {
            int64_t v;
            if (parse_number(p, &v) != 0) {
                snprintf(a->err, sizeof(a->err), "line %d: expected number", line_no);
                return -1;
            }
            uint8_t b = (uint8_t)(v & 0xFF);
            buf_append(current_buf(a), &b, 1);
            return 0;
        }
        if (strcmp(mnem, ".word") == 0) {
            return emit_operand(a, p, line_no);
        }
        if (strcmp(mnem, ".ascii") == 0 || strcmp(mnem, ".asciz") == 0) {
            char str[8192];
            size_t used;
            if (parse_string(p, str, sizeof(str), &used) != 0) {
                snprintf(a->err, sizeof(a->err), "line %d: bad string", line_no);
                return -1;
            }
            buf_append(current_buf(a), (uint8_t *)str, used);
            if (strcmp(mnem, ".asciz") == 0) {
                uint8_t zero = 0;
                buf_append(current_buf(a), &zero, 1);
            }
            return 0;
        }
        if (strcmp(mnem, ".org") == 0) {
            int64_t v;
            if (parse_number(p, &v) != 0) {
                snprintf(a->err, sizeof(a->err), "line %d: expected number", line_no);
                return -1;
            }
            ByteBuf *buf = current_buf(a);
            if ((uint32_t)v < buf->len) {
                snprintf(a->err, sizeof(a->err), "line %d: .org cannot move backwards", line_no);
                return -1;
            }
            while (buf->len < (size_t)v) {
                uint8_t z = 0;
                buf_append(buf, &z, 1);
            }
            return 0;
        }
        snprintf(a->err, sizeof(a->err), "line %d: unknown directive '%s'", line_no, mnem);
        return -1;
    }

    /* Instructions. */
    ZeusOpcode op = zeus_op_by_name(mnem);
    if (op == OP_COUNT) {
        snprintf(a->err, sizeof(a->err), "line %d: unknown opcode '%s'", line_no, mnem);
        return -1;
    }

    const ZeusOpInfo *info = zeus_op_info(op);
    uint8_t op_byte = (uint8_t)op;
    buf_append(current_buf(a), &op_byte, 1);

    if (info->has_operand) {
        /* Operand may be separated by whitespace; p already points to it. */
        if (!*p || *p == ';') {
            snprintf(a->err, sizeof(a->err), "line %d: missing operand for %s", line_no, info->name);
            return -1;
        }
        /* Operand ends at whitespace or comment. */
        char *end = p;
        while (*end && !isspace((unsigned char)*end) && *end != ';') end++;
        size_t op_len = (size_t)(end - p);
        char operand[128];
        if (op_len >= sizeof(operand)) op_len = sizeof(operand) - 1;
        memcpy(operand, p, op_len);
        operand[op_len] = '\0';

        if (emit_operand(a, operand, line_no) != 0) {
            if (a->err[0] == '\0') {
                snprintf(a->err, sizeof(a->err), "line %d: bad operand '%s'", line_no, operand);
            }
            return -1;
        }
    }

    return 0;
}

static int resolve_fixups(Assembler *a) {
    for (int i = 0; i < a->fix_count; i++) {
        Fixup *f = &a->fixups[i];
        Symbol *s = find_symbol(a, f->name);
        if (!s) {
            snprintf(a->err, sizeof(a->err), "line %d: undefined symbol '%s'", f->line, f->name);
            return -1;
        }
        ByteBuf *buf = (f->section == SEC_CODE) ? &a->code : &a->data;
        if (f->offset + 8 > buf->len) {
            snprintf(a->err, sizeof(a->err), "line %d: fixup out of range", f->line);
            return -1;
        }
        int64_t value = (int64_t)s->offset;
        uint8_t *p = buf->buf + f->offset;
        uint64_t u = (uint64_t)value;
        p[0] = (uint8_t)(u & 0xFF);
        p[1] = (uint8_t)((u >> 8) & 0xFF);
        p[2] = (uint8_t)((u >> 16) & 0xFF);
        p[3] = (uint8_t)((u >> 24) & 0xFF);
        p[4] = (uint8_t)((u >> 32) & 0xFF);
        p[5] = (uint8_t)((u >> 40) & 0xFF);
        p[6] = (uint8_t)((u >> 48) & 0xFF);
        p[7] = (uint8_t)((u >> 56) & 0xFF);
    }
    return 0;
}

ZeusError zeus_assemble(const char *source, ZeusProgram *out_prog, char *err_buf, size_t err_len) {
    if (!source || !out_prog) return ZEUS_ERR_IO;

    Assembler a;
    asm_init(&a);

    char *src = strdup(source);
    char *line = src;
    int line_no = 1;

    while (*line) {
        char *end = strchr(line, '\n');
        char saved = '\0';
        if (end) {
            saved = *end;
            *end = '\0';
        }

        if (process_line(&a, line, line_no) != 0) {
            if (err_buf && err_len > 0) {
                strncpy(err_buf, a.err, err_len - 1);
                err_buf[err_len - 1] = '\0';
            }
            free(src);
            asm_free(&a);
            return ZEUS_ERR_IO;
        }

        if (!end) break;
        *end = saved;
        line = end + 1;
        line_no++;
    }

    if (resolve_fixups(&a) != 0) {
        if (err_buf && err_len > 0) {
            strncpy(err_buf, a.err, err_len - 1);
            err_buf[err_len - 1] = '\0';
        }
        free(src);
        asm_free(&a);
        return ZEUS_ERR_IO;
    }

    zeus_program_init(out_prog);
    out_prog->header.magic[0] = ZEUS_MAGIC[0];
    out_prog->header.magic[1] = ZEUS_MAGIC[1];
    out_prog->header.magic[2] = ZEUS_MAGIC[2];
    out_prog->header.magic[3] = ZEUS_MAGIC[3];
    out_prog->header.version = ZEUS_VERSION;
    out_prog->header.entry_point = 0;
    out_prog->header.code_len = (uint32_t)a.code.len;
    out_prog->header.data_len = (uint32_t)a.data.len;

    if (a.code.len > 0) {
        out_prog->code = (uint8_t *)malloc(a.code.len);
        if (!out_prog->code) {
            free(src);
            asm_free(&a);
            return ZEUS_ERR_IO;
        }
        memcpy(out_prog->code, a.code.buf, a.code.len);
    }
    if (a.data.len > 0) {
        out_prog->data = (uint8_t *)malloc(a.data.len);
        if (!out_prog->data) {
            free(src);
            asm_free(&a);
            return ZEUS_ERR_IO;
        }
        memcpy(out_prog->data, a.data.buf, a.data.len);
    }

    free(src);
    asm_free(&a);
    return ZEUS_OK;
}
