#define _POSIX_C_SOURCE 200809L

#include "snapshot.h"
#include "net.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#ifdef _WIN32
#include <io.h>
#define zsnap_fsync _commit
#else
#include <unistd.h>
#define zsnap_fsync fsync
#endif

/* ------------------------------------------------------------------ */
/* Format (format version 3)                                           */
/*                                                                     */
/*    8   magic "ZSNAP3\0\0"                                           */
/*   14 x u32  format, code_len, data_len, entry, bc_version, error,   */
/*             pc, started, running, yielded, pending_kind,            */
/*             pending_buf, pending_len, packet_count                  */
/*    5 x i64  sp, csp, pending_fd, pending_id, pending_value          */
/*    33  lineage (NUL-padded; empty = unbound)                        */
/*    8   u64  seq (pending effect number; 0 = unbound)                */
/*    8   u64  journal_head (reserved for the journal chain; 0)        */
/*    packets  per used slot: index, size, len, proto, src, dst,       */
/*             payload[size]                                           */
/*    stack    ZEUS_STACK_SIZE * 8                                     */
/*    calls    ZEUS_CALL_SIZE * 4                                      */
/*    memory   ZEUS_MEMORY_SIZE                                        */
/*    code     code_len                                                */
/*    data     data_len                                                */
/*    8   u64  FNV-1a checksum of every preceding byte                 */
/*                                                                     */
/* `yielded` is the only "is there a pending effect?" bit; when it is  */
/* zero every pending field must be zero. Packet entries carry their   */
/* slot index so a fork keeps raw-packet ids stable even when a lower  */
/* slot was freed.                                                     */
/*                                                                     */
/* Everything is little-endian and written field by field, so the file */
/* layout does not depend on the compiler's struct padding.            */
/* ------------------------------------------------------------------ */

#define ZSNAP_FORMAT_VERSION 3u
#define ZSNAP_HASH_SIZE 8u
#define ZSNAP_HEADER_U32S 14u
#define ZSNAP_HEADER_I64S 5u
#define ZSNAP_META_SIZE (ZEUS_SNAPSHOT_LINEAGE_LEN + 16u)
#define ZSNAP_MIN_SIZE (sizeof(ZSNAP_MAGIC) + ZSNAP_HEADER_U32S * 4u \
                        + ZSNAP_HEADER_I64S * 8u + ZSNAP_META_SIZE \
                        + ZSNAP_HASH_SIZE)

static const uint8_t ZSNAP_MAGIC[8] = { 'Z', 'S', 'N', 'A', 'P', '3', 0, 0 };

/* ------------------------------ write side ------------------------- */

typedef struct {
    uint8_t *p;
    size_t len;
    size_t cap;
    bool ok;
} Buf;

static bool buf_reserve(Buf *b, size_t extra) {
    if (b->len + extra <= b->cap) return true;
    size_t nc = b->cap ? b->cap : 4096;
    while (nc < b->len + extra) {
        if (nc > (SIZE_MAX / 2)) { b->ok = false; return false; }
        nc *= 2;
    }
    uint8_t *np = (uint8_t *)realloc(b->p, nc);
    if (!np) { b->ok = false; return false; }
    b->p = np;
    b->cap = nc;
    return true;
}

static void put_bytes(Buf *b, const void *src, size_t n) {
    if (!b->ok || !buf_reserve(b, n)) return;
    memcpy(b->p + b->len, src, n);
    b->len += n;
}

static void put_u32(Buf *b, uint32_t v) {
    uint8_t p[4];
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
    put_bytes(b, p, 4);
}

static void put_i64(Buf *b, int64_t v) {
    uint64_t u = (uint64_t)v;
    uint8_t p[8];
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)((u >> (8 * i)) & 0xFF);
    put_bytes(b, p, 8);
}

static uint64_t fnv1a(const uint8_t *p, size_t n) {
    uint64_t h = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}

static bool packet_slot_ok(const ZeusPacket *pk) {
    return pk->buf != NULL &&
           pk->size > 0 && pk->size <= ZEUS_MAX_PACKET_SIZE &&
           pk->len <= pk->size;
}

static bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static bool snapshot_lineage_ok(const char *lineage) {
    if (!lineage || !*lineage) return true;
    if (strlen(lineage) != 32) return false;
    for (size_t i = 0; i < 32; i++) {
        if (!is_hex_digit(lineage[i])) return false;
    }
    return true;
}

/* Write the whole file through a same-directory temporary and rename it into
 * place. The destination either keeps its previous contents or becomes the
 * complete new snapshot; a crash cannot leave a half-written file under the
 * name a dossier points at. */
static bool write_file_atomic(const char *path, const uint8_t *data,
                              size_t len) {
    if (!path || !*path) return false;

    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    {
        const char *bslash = strrchr(path, '\\');
        if (bslash && (!slash || bslash > slash)) slash = bslash;
    }
#endif

    char *dir = NULL;
    const char *base = path;
    if (!slash) {
        dir = (char *)malloc(2);
        if (!dir) return false;
        memcpy(dir, ".", 2);
    } else {
        size_t n = (size_t)(slash - path);
        if (n == 0) n = 1;          /* the file sits in the root directory */
        dir = (char *)malloc(n + 1);
        if (!dir) return false;
        memcpy(dir, path, n);
        dir[n] = '\0';
        base = slash + 1;
    }
    if (*base == '\0') {
        free(dir);
        return false;
    }

    size_t need = strlen(dir) + 1 + strlen(base) + sizeof(".tmp.XXXXXX");
    char *tmp = (char *)malloc(need);
    if (!tmp) {
        free(dir);
        return false;
    }
    int written = snprintf(tmp, need, "%s/%s.tmp.XXXXXX", dir, base);
    if (written < 0 || (size_t)written >= need) {
        free(tmp);
        free(dir);
        return false;
    }

    int fd = mkstemp(tmp);
    if (fd < 0) {
        free(tmp);
        free(dir);
        return false;
    }

    bool ok = true;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        if (n == 0) {
            ok = false;
            break;
        }
        off += (size_t)n;
    }
    if (ok && zsnap_fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;

    if (ok && rename(tmp, path) != 0) ok = false;

    if (!ok) {
        unlink(tmp);
        free(tmp);
        free(dir);
        return false;
    }

#ifndef _WIN32
    /* Make the directory entry durable too: without this a crash can leave
     * the old name pointing at the old file after the new inode was synced. */
    int dfd = open(dir, O_RDONLY);
    if (dfd >= 0) {
        int ignored = fsync(dfd);
        (void)ignored;
        close(dfd);
    }
#endif

    free(tmp);
    free(dir);
    return true;
}

/* The writer and the reader enforce the same invariants, so a snapshot is
 * either a state execution can produce or no file at all. */
static bool snapshot_state_ok(const ZeusVM *vm) {
    if (!vm) return false;

    const ZeusProgram *prog = &vm->program;
    if (prog->header.code_len == 0 || prog->header.code_len > ZEUS_MAX_CODE_SIZE) return false;
    if (!prog->code) return false;
    if (prog->header.data_len > ZEUS_MEMORY_SIZE) return false;
    if (prog->header.data_len > 0 && !prog->data) return false;
    if (prog->header.entry_point > prog->header.code_len) return false;

    if (vm->pc > prog->header.code_len) return false;
    if (vm->sp < 0 || vm->sp > ZEUS_STACK_SIZE) return false;
    if (vm->csp < 0 || vm->csp > ZEUS_CALL_SIZE) return false;
    if (vm->running && (!vm->started || vm->error != ZEUS_OK)) return false;

    if (vm->yielded) {
        if (!vm->running) return false;
        if (vm->pending.kind == ZEUS_EFFECT_NONE) return false;
        if (!zeus_effect_operands_valid(vm, &vm->pending)) return false;
    } else {
        const ZeusEffect *eff = &vm->pending;
        if (eff->kind != ZEUS_EFFECT_NONE || eff->fd != 0 || eff->id != 0 ||
            eff->value != 0 || eff->buf != 0 || eff->len != 0) {
            return false;
        }
    }

    if (vm->net) {
        for (int i = 0; i < ZEUS_MAX_PACKETS; i++) {
            if (vm->net->packets[i].used && !packet_slot_ok(&vm->net->packets[i])) {
                return false;
            }
        }
    }

    return true;
}

static ZeusError snapshot_write(const ZeusVM *vm, const char *path,
                                const ZeusSnapshotMeta *meta) {
    if (!vm || !path || !meta) return ZEUS_ERR_IO;
    if (!snapshot_lineage_ok(meta->lineage)) return ZEUS_ERR_SNAPSHOT;
    if (!snapshot_state_ok(vm)) return ZEUS_ERR_SNAPSHOT;

    uint32_t code_len = vm->program.header.code_len;
    uint32_t data_len = vm->program.header.data_len;

    uint32_t packet_count = 0;
    if (vm->net) {
        for (int i = 0; i < ZEUS_MAX_PACKETS; i++) {
            if (vm->net->packets[i].used) packet_count++;
        }
    }

    Buf b;
    memset(&b, 0, sizeof(b));
    b.ok = true;

    put_bytes(&b, ZSNAP_MAGIC, sizeof(ZSNAP_MAGIC));
    put_u32(&b, ZSNAP_FORMAT_VERSION);
    put_u32(&b, code_len);
    put_u32(&b, data_len);
    put_u32(&b, vm->program.header.entry_point);
    put_u32(&b, (uint32_t)vm->program.header.version);
    put_u32(&b, (uint32_t)vm->error);
    put_u32(&b, vm->pc);
    put_u32(&b, vm->started ? 1u : 0u);
    put_u32(&b, vm->running ? 1u : 0u);
    put_u32(&b, vm->yielded ? 1u : 0u);
    put_u32(&b, (uint32_t)vm->pending.kind);
    put_u32(&b, vm->pending.buf);
    put_u32(&b, vm->pending.len);
    put_u32(&b, packet_count);

    put_i64(&b, vm->sp);
    put_i64(&b, vm->csp);
    put_i64(&b, vm->pending.fd);
    put_i64(&b, vm->pending.id);
    put_i64(&b, vm->pending.value);

    put_bytes(&b, meta->lineage, ZEUS_SNAPSHOT_LINEAGE_LEN);
    put_i64(&b, (int64_t)meta->seq);
    put_i64(&b, (int64_t)meta->journal_head);

    if (vm->net) {
        for (int i = 0; i < ZEUS_MAX_PACKETS; i++) {
            const ZeusPacket *pk = &vm->net->packets[i];
            if (!pk->used) continue;
            put_u32(&b, (uint32_t)i);       /* slot id, not a compact index */
            put_u32(&b, pk->size);
            put_u32(&b, pk->len);
            put_u32(&b, (uint32_t)pk->proto);
            put_u32(&b, pk->src);
            put_u32(&b, pk->dst);
            put_bytes(&b, pk->buf, pk->size);
        }
    }

    put_bytes(&b, vm->stack, sizeof(vm->stack));
    put_bytes(&b, vm->call_stack, sizeof(vm->call_stack));
    put_bytes(&b, vm->memory, sizeof(vm->memory));
    put_bytes(&b, vm->program.code, code_len);
    if (data_len > 0) put_bytes(&b, vm->program.data, data_len);

    if (!b.ok) {
        free(b.p);
        return ZEUS_ERR_IO;
    }

    uint64_t hash = fnv1a(b.p, b.len);
    put_i64(&b, (int64_t)hash);
    if (!b.ok) {
        free(b.p);
        return ZEUS_ERR_IO;
    }

    bool wrote = write_file_atomic(path, b.p, b.len);
    free(b.p);
    return wrote ? ZEUS_OK : ZEUS_ERR_IO;
}

ZeusError zeus_vm_snapshot(const ZeusVM *vm, const char *path) {
    ZeusSnapshotMeta meta;
    memset(&meta, 0, sizeof(meta));
    return snapshot_write(vm, path, &meta);
}

ZeusError zeus_vm_snapshot_bound_ex(const ZeusVM *vm, const char *path,
                                    const char *lineage, uint64_t seq,
                                    uint64_t journal_head) {
    /* Validate before copying: snprintf would truncate a too-long id into a
     * different, apparently well-formed lineage. */
    if (!snapshot_lineage_ok(lineage)) return ZEUS_ERR_SNAPSHOT;

    ZeusSnapshotMeta meta;
    memset(&meta, 0, sizeof(meta));
    if (lineage) snprintf(meta.lineage, sizeof(meta.lineage), "%s", lineage);
    meta.seq = seq;
    meta.journal_head = journal_head;
    return snapshot_write(vm, path, &meta);
}

ZeusError zeus_vm_snapshot_bound(const ZeusVM *vm, const char *path,
                                 const char *lineage, uint64_t seq) {
    return zeus_vm_snapshot_bound_ex(vm, path, lineage, seq, 0);
}

/* ------------------------------ read side -------------------------- */

typedef struct {
    const uint8_t *p;
    size_t len;
    size_t off;
    bool ok;
} Reader;

static bool rd_bytes(Reader *r, void *dst, size_t n) {
    if (!r->ok || n > r->len || r->off > r->len - n) { r->ok = false; return false; }
    if (dst) memcpy(dst, r->p + r->off, n);
    r->off += n;
    return true;
}

static bool rd_u32(Reader *r, uint32_t *v) {
    uint8_t p[4];
    if (!rd_bytes(r, p, 4)) return false;
    *v = (uint32_t)p[0]
       | ((uint32_t)p[1] << 8)
       | ((uint32_t)p[2] << 16)
       | ((uint32_t)p[3] << 24);
    return true;
}

static bool rd_i64(Reader *r, int64_t *v) {
    uint8_t p[8];
    if (!rd_bytes(r, p, 8)) return false;
    uint64_t u = 0;
    for (int i = 0; i < 8; i++) u |= ((uint64_t)p[i] << (8 * i));
    *v = (int64_t)u;
    return true;
}

static uint8_t *read_whole_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);

    uint8_t *p = (uint8_t *)malloc((size_t)n ? (size_t)n : 1);
    if (!p) { fclose(f); return NULL; }

    if (n > 0 && fread(p, 1, (size_t)n, f) != (size_t)n) {
        free(p);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)n;
    return p;
}

static bool read_lineage(const uint8_t raw[ZEUS_SNAPSHOT_LINEAGE_LEN],
                         char out[ZEUS_SNAPSHOT_LINEAGE_LEN]) {
    size_t n = 0;
    while (n < ZEUS_SNAPSHOT_LINEAGE_LEN && raw[n] != 0) n++;
    if (n == ZEUS_SNAPSHOT_LINEAGE_LEN) return false;
    for (size_t i = n; i < ZEUS_SNAPSHOT_LINEAGE_LEN; i++) {
        if (raw[i] != 0) return false;
    }
    if (n != 0 && n != 32) return false;
    for (size_t i = 0; i < n; i++) {
        if (!is_hex_digit((char)raw[i])) return false;
    }
    memcpy(out, raw, n);
    out[n] = '\0';
    return true;
}

ZeusError zeus_vm_restore_ex(ZeusVM *vm, const char *path,
                             ZeusSnapshotMeta *out) {
    if (!vm || !path) return ZEUS_ERR_IO;

    size_t file_len = 0;
    uint8_t *file = read_whole_file(path, &file_len);
    if (!file) return ZEUS_ERR_IO;

    if (file_len < ZSNAP_MIN_SIZE) {
        free(file);
        return ZEUS_ERR_SNAPSHOT;
    }

    size_t body_len = file_len - ZSNAP_HASH_SIZE;

    {
        Reader hr;
        hr.p = file + body_len;
        hr.len = ZSNAP_HASH_SIZE;
        hr.off = 0;
        hr.ok = true;

        int64_t stored_hash = 0;
        if (!rd_i64(&hr, &stored_hash) || (uint64_t)stored_hash != fnv1a(file, body_len)) {
            free(file);
            return ZEUS_ERR_SNAPSHOT;
        }
    }

    Reader r;
    r.p = file;
    r.len = body_len;
    r.off = 0;
    r.ok = true;

    uint8_t magic[8];
    uint32_t format = 0, code_len = 0, data_len = 0, entry = 0, bc_version = 0;
    uint32_t error = 0, pc = 0, started = 0, running = 0, yielded = 0;
    uint32_t pending_kind = 0, pending_buf = 0, pending_len = 0, packet_count = 0;
    int64_t sp = 0, csp = 0, pending_fd = 0, pending_id = 0, pending_value = 0;
    uint8_t meta_lineage[ZEUS_SNAPSHOT_LINEAGE_LEN];
    memset(meta_lineage, 0, sizeof(meta_lineage));
    int64_t meta_seq = 0, meta_head = 0;

    bool parsed =
        rd_bytes(&r, magic, sizeof(magic)) &&
        rd_u32(&r, &format) &&
        rd_u32(&r, &code_len) &&
        rd_u32(&r, &data_len) &&
        rd_u32(&r, &entry) &&
        rd_u32(&r, &bc_version) &&
        rd_u32(&r, &error) &&
        rd_u32(&r, &pc) &&
        rd_u32(&r, &started) &&
        rd_u32(&r, &running) &&
        rd_u32(&r, &yielded) &&
        rd_u32(&r, &pending_kind) &&
        rd_u32(&r, &pending_buf) &&
        rd_u32(&r, &pending_len) &&
        rd_u32(&r, &packet_count) &&
        rd_i64(&r, &sp) &&
        rd_i64(&r, &csp) &&
        rd_i64(&r, &pending_fd) &&
        rd_i64(&r, &pending_id) &&
        rd_i64(&r, &pending_value) &&
        rd_bytes(&r, meta_lineage, sizeof(meta_lineage)) &&
        rd_i64(&r, &meta_seq) &&
        rd_i64(&r, &meta_head);

    ZeusSnapshotMeta meta;
    memset(&meta, 0, sizeof(meta));
    bool meta_ok = parsed && read_lineage(meta_lineage, meta.lineage);
    if (meta_ok) {
        meta.seq = (uint64_t)meta_seq;
        meta.journal_head = (uint64_t)meta_head;
    }

    bool pending_zero = pending_kind == (uint32_t)ZEUS_EFFECT_NONE &&
                        pending_buf == 0 && pending_len == 0 &&
                        pending_fd == 0 && pending_id == 0 && pending_value == 0;

    bool valid = parsed && meta_ok
        && memcmp(magic, ZSNAP_MAGIC, sizeof(magic)) == 0
        && format == ZSNAP_FORMAT_VERSION
        && bc_version == ZEUS_VERSION
        && code_len > 0 && code_len <= ZEUS_MAX_CODE_SIZE
        && data_len <= ZEUS_MEMORY_SIZE
        && entry <= code_len
        && pc <= code_len
        && error <= (uint32_t)ZEUS_ERR_SNAPSHOT
        && started <= 1u && running <= 1u && yielded <= 1u
        && running <= started
        && (!running || error == (uint32_t)ZEUS_OK)
        && pending_kind < (uint32_t)ZEUS_EFFECT_KIND_COUNT
        && (yielded ? (pending_kind != (uint32_t)ZEUS_EFFECT_NONE && running == 1u)
                    : pending_zero)
        && sp >= 0 && sp <= ZEUS_STACK_SIZE
        && csp >= 0 && csp <= ZEUS_CALL_SIZE
        && packet_count <= ZEUS_MAX_PACKETS;

    if (!valid) {
        free(file);
        return ZEUS_ERR_SNAPSHOT;
    }

    ZeusVM tmp;
    zeus_vm_init(&tmp);
    if (!tmp.net) {
        free(file);
        return ZEUS_ERR_IO;
    }

    bool ok = true;

    for (uint32_t i = 0; i < packet_count && ok; i++) {
        uint32_t index = 0, size = 0, len = 0, proto = 0, src = 0, dst = 0;

        ok = rd_u32(&r, &index) && rd_u32(&r, &size) && rd_u32(&r, &len) &&
             rd_u32(&r, &proto) && rd_u32(&r, &src) && rd_u32(&r, &dst);
        if (!ok || index >= ZEUS_MAX_PACKETS || tmp.net->packets[index].used ||
            size == 0 || size > ZEUS_MAX_PACKET_SIZE || len > size) {
            ok = false;
            break;
        }

        uint8_t *buf = (uint8_t *)malloc(size);
        if (!buf) { ok = false; break; }
        if (!rd_bytes(&r, buf, size)) {
            free(buf);
            ok = false;
            break;
        }

        ZeusPacket *pk = &tmp.net->packets[index];
        pk->used = true;
        pk->buf = buf;
        pk->size = size;
        pk->len = len;
        pk->proto = (int)proto;
        pk->src = src;
        pk->dst = dst;
    }

    if (ok) ok = rd_bytes(&r, tmp.stack, sizeof(tmp.stack));
    if (ok) ok = rd_bytes(&r, tmp.call_stack, sizeof(tmp.call_stack));
    if (ok) ok = rd_bytes(&r, tmp.memory, sizeof(tmp.memory));

    if (ok) {
        memcpy(tmp.program.header.magic, ZEUS_MAGIC, ZEUS_MAGIC_LEN);
        tmp.program.header.version = (uint8_t)bc_version;
        tmp.program.header.entry_point = entry;
        tmp.program.header.code_len = code_len;
        tmp.program.header.data_len = data_len;

        tmp.program.code = (uint8_t *)malloc(code_len);
        if (!tmp.program.code) ok = false;
        else ok = rd_bytes(&r, tmp.program.code, code_len);
    }

    if (ok && data_len > 0) {
        tmp.program.data = (uint8_t *)malloc(data_len);
        if (!tmp.program.data) ok = false;
        else ok = rd_bytes(&r, tmp.program.data, data_len);
    }

    ZeusEffect pending = {
        .kind = (ZeusEffectKind)pending_kind,
        .fd = pending_fd,
        .id = pending_id,
        .value = pending_value,
        .buf = pending_buf,
        .len = pending_len,
    };

    /* Memory is loaded now, so string/range operands can be checked exactly
     * the way the interpreter checked them before yielding. */
    if (ok && r.off != r.len) ok = false;
    if (ok && yielded == 1u && !zeus_effect_operands_valid(&tmp, &pending)) ok = false;

    if (!ok) {
        free(file);
        zeus_vm_free(&tmp);
        return ZEUS_ERR_SNAPSHOT;
    }

    tmp.pc = pc;
    tmp.sp = sp;
    tmp.csp = csp;
    tmp.error = (ZeusError)error;
    tmp.started = (started == 1u);
    tmp.running = (running == 1u);
    tmp.yielded = (yielded == 1u);
    tmp.pending = pending;

    free(file);
    zeus_vm_free(vm);   /* release whatever the caller had loaded */
    *vm = tmp;
    if (out) *out = meta;
    return ZEUS_OK;
}

ZeusError zeus_vm_restore(ZeusVM *vm, const char *path) {
    return zeus_vm_restore_ex(vm, path, NULL);
}
