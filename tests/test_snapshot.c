#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <dirent.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include "zeus.h"
#include "vm.h"
#include "../src/net.h"   /* packet-slot internals, for the fork test */
#include "asm.h"
#include "snapshot.h"

static int tests_run = 0;
static int tests_failed = 0;

#define RUN_TEST(name) do { \
    printf("  [test] " #name " ... "); \
    tests_run++; \
    if (name()) { \
        printf("OK\n"); \
    } else { \
        printf("FAIL\n"); \
        tests_failed++; \
    } \
} while (0)

/* Snapshot header offsets (shared by formats 2 and 3): 8-byte magic, then
 * u32 fields. */
enum {
    SNAP_OFF_FORMAT = 8,
    SNAP_OFF_CODE_LEN = 12,
    SNAP_OFF_DATA_LEN = 16,
    SNAP_OFF_ENTRY = 20,
    SNAP_OFF_BC_VERSION = 24,
    SNAP_OFF_ERROR = 28,
    SNAP_OFF_PC = 32,
    SNAP_OFF_STARTED = 36,
    SNAP_OFF_RUNNING = 40,
    SNAP_OFF_YIELDED = 44,
    SNAP_OFF_PENDING_KIND = 48,
    SNAP_OFF_PENDING_BUF = 52,
    SNAP_OFF_PENDING_LEN = 56,
    SNAP_OFF_PACKET_COUNT = 60
};

/* mem[100] = 5, then mem[100] = 7, then push 42 and halt. STOREI pops the
 * address first, so the value is pushed before it. Three instructions leave
 * the machine at a clean mid-program point with an observable state. */
static const char *PROG =
    ".code\n"
    "PUSH 5\n"
    "PUSH 100\n"
    "STOREI\n"
    "PUSH 7\n"
    "PUSH 100\n"
    "STOREI\n"
    "PUSH 42\n"
    "HALT\n";

static int build(ZeusProgram *prog, ZeusVM *vm) {
    char err[256];
    zeus_program_init(prog);
    if (zeus_assemble(PROG, prog, err, sizeof(err)) != ZEUS_OK) {
        fprintf(stderr, "asm error: %s\n", err);
        return 0;
    }
    zeus_vm_init(vm);
    if (zeus_vm_load(vm, prog) != ZEUS_OK) {
        return 0;
    }
    zeus_vm_start(vm);
    return 1;
}

static int exec_n(ZeusVM *vm, int n) {
    for (int i = 0; i < n; i++) {
        ZeusEffect eff;
        if (zeus_vm_exec(vm, &eff) != ZEUS_OK) return 0;
        if (vm->yielded) return 0;
    }
    return 1;
}

static int run_to_end(ZeusVM *vm) {
    while (vm->running) {
        ZeusEffect eff;
        ZeusError err = zeus_vm_exec(vm, &eff);
        if (err != ZEUS_OK) return 0;
        if (vm->yielded) return 0;
    }
    return vm->error == ZEUS_OK;
}

static int restore_into(ZeusVM *vm, const char *path) {
    zeus_vm_init(vm);
    return zeus_vm_restore(vm, path) == ZEUS_OK;
}

/* ---- patching a snapshot while keeping its checksum valid ---- */

static uint64_t fnv1a(const uint8_t *p, size_t n) {
    uint64_t h = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}

typedef struct {
    size_t off;
    uint32_t value;
} Patch;

/* Copy `src` to `dst`, overwrite u32 fields at the given offsets, and
 * recompute the trailing checksum. This simulates a snapshot that is
 * internally consistent by checksum but semantically invalid. */
static int patch_snapshot(const char *src, const char *dst,
                          const Patch *patches, size_t count) {
    FILE *f = fopen(src, "rb");
    if (!f) return 0;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long n = ftell(f);
    if (n < 8) { fclose(f); return 0; }
    rewind(f);

    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { fclose(f); return 0; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); return 0; }

    for (size_t i = 0; i < count; i++) {
        if (patches[i].off + 4 > (size_t)n - 8) { free(buf); return 0; }
        uint32_t v = patches[i].value;
        buf[patches[i].off + 0] = (uint8_t)(v & 0xFF);
        buf[patches[i].off + 1] = (uint8_t)((v >> 8) & 0xFF);
        buf[patches[i].off + 2] = (uint8_t)((v >> 16) & 0xFF);
        buf[patches[i].off + 3] = (uint8_t)((v >> 24) & 0xFF);
    }

    uint64_t h = fnv1a(buf, (size_t)n - 8);
    for (int i = 0; i < 8; i++) {
        buf[(size_t)n - 8 + i] = (uint8_t)((h >> (8 * i)) & 0xFF);
    }

    f = fopen(dst, "wb");
    if (!f) { free(buf); return 0; }
    size_t written = fwrite(buf, 1, (size_t)n, f);
    int closed = fclose(f);
    free(buf);

    return written == (size_t)n && closed == 0;
}

static int rejects_file(const char *path) {
    ZeusVM vm;
    zeus_vm_init(&vm);
    ZeusError e = zeus_vm_restore(&vm, path);
    zeus_vm_free(&vm);
    return e == ZEUS_ERR_SNAPSHOT;
}

static int rejected_after_patch(const char *src, const char *dst,
                                const Patch *patches, size_t count) {
    if (!patch_snapshot(src, dst, patches, count)) return 0;
    return rejects_file(dst);
}

/* Overwrite one byte and recompute the checksum, so the file stays
 * self-consistent and only the field under test can justify a rejection. */
static int patch_snapshot_byte(const char *src, const char *dst,
                               size_t off, uint8_t value) {
    FILE *f = fopen(src, "rb");
    if (!f) return 0;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long n = ftell(f);
    if (n < 8) { fclose(f); return 0; }
    rewind(f);

    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { fclose(f); return 0; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n || off >= (size_t)n - 8) { free(buf); return 0; }

    buf[off] = value;
    uint64_t h = fnv1a(buf, (size_t)n - 8);
    for (int i = 0; i < 8; i++) {
        buf[(size_t)n - 8 + i] = (uint8_t)((h >> (8 * i)) & 0xFF);
    }

    f = fopen(dst, "wb");
    if (!f) { free(buf); return 0; }
    size_t written = fwrite(buf, 1, (size_t)n, f);
    int closed = fclose(f);
    free(buf);
    return written == (size_t)n && closed == 0;
}

static int files_identical(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb");
    FILE *fb = fopen(b, "rb");
    if (!fa || !fb) {
        if (fa) fclose(fa);
        if (fb) fclose(fb);
        return 0;
    }

    int same = 1;
    for (;;) {
        int ca = fgetc(fa);
        int cb = fgetc(fb);
        if (ca != cb) { same = 0; break; }
        if (ca == EOF) break;
    }
    fclose(fa);
    fclose(fb);
    return same;
}

static int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    FILE *out = fopen(dst, "wb");
    if (!in || !out) {
        if (in) fclose(in);
        if (out) fclose(out);
        return 0;
    }

    int ok = 1;
    int ch;
    while ((ch = fgetc(in)) != EOF) {
        if (fputc(ch, out) == EOF) { ok = 0; break; }
    }
    if (fclose(in) != 0) ok = 0;
    if (fclose(out) != 0) ok = 0;
    return ok;
}

static int dir_has_temp(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 1;
    int found = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strstr(ent->d_name, ".tmp.") != NULL) { found = 1; break; }
    }
    closedir(d);
    return found;
}

static int test_roundtrip_resumes_exactly(void) {
    const char *p1 = "/tmp/zeus_snap_roundtrip.zsnap";

    ZeusProgram prog;
    ZeusVM a;
    if (!build(&prog, &a)) return 0;

    int ok = exec_n(&a, 3);
    ok = ok && (a.memory[100] == 5);
    ok = ok && (zeus_vm_snapshot(&a, p1) == ZEUS_OK);

    ok = ok && run_to_end(&a);

    ZeusVM b;
    ok = ok && restore_into(&b, p1);
    ok = ok && run_to_end(&b);

    ok = ok && (a.error == b.error);
    ok = ok && (a.sp == b.sp);
    ok = ok && (a.pc == b.pc);
    ok = ok && (a.memory[100] == 7);
    ok = ok && (b.memory[100] == 7);
    ok = ok && (zeus_vm_peek(&a, 0) == 42);
    ok = ok && (zeus_vm_peek(&b, 0) == 42);

    zeus_vm_free(&b);
    zeus_vm_free(&a);
    zeus_program_free(&prog);
    remove(p1);
    return ok;
}

static int test_snapshot_is_byte_stable(void) {
    const char *p1 = "/tmp/zeus_snap_stable_a.zsnap";
    const char *p2 = "/tmp/zeus_snap_stable_b.zsnap";

    ZeusProgram prog;
    ZeusVM a;
    if (!build(&prog, &a)) return 0;

    int ok = exec_n(&a, 3);
    ok = ok && (zeus_vm_snapshot(&a, p1) == ZEUS_OK);

    ZeusVM b;
    ok = ok && restore_into(&b, p1);
    ok = ok && (zeus_vm_snapshot(&b, p2) == ZEUS_OK);

    /* Same machine state must serialize to identical bytes. */
    FILE *fa = fopen(p1, "rb");
    FILE *fb = fopen(p2, "rb");
    ok = ok && fa && fb;

    if (ok) {
        int ch;
        do {
            ch = fgetc(fa);
            if (ch != fgetc(fb)) { ok = 0; break; }
        } while (ch != EOF);
    }

    if (fa) fclose(fa);
    if (fb) fclose(fb);

    zeus_vm_free(&b);
    zeus_vm_free(&a);
    zeus_program_free(&prog);
    remove(p1);
    remove(p2);
    return ok;
}

static int test_fork_gives_independent_machines(void) {
    const char *p1 = "/tmp/zeus_snap_fork.zsnap";

    ZeusProgram prog;
    ZeusVM a;
    if (!build(&prog, &a)) return 0;

    int ok = exec_n(&a, 3);
    ok = ok && (zeus_vm_snapshot(&a, p1) == ZEUS_OK);

    ZeusVM f1, f2;
    ok = ok && restore_into(&f1, p1);
    ok = ok && restore_into(&f2, p1);

    ok = ok && run_to_end(&f1);          /* branch one runs to completion */

    /* The second machine is untouched by the first one's execution. */
    ok = ok && (f1.memory[100] == 7);
    ok = ok && (f2.memory[100] == 5);
    ok = ok && (f2.pc != f1.pc);

    zeus_vm_free(&f2);
    zeus_vm_free(&f1);
    zeus_vm_free(&a);
    zeus_program_free(&prog);
    remove(p1);
    return ok;
}

static int test_tampered_snapshot_rejected(void) {
    const char *p1 = "/tmp/zeus_snap_tamper.zsnap";

    ZeusProgram prog;
    ZeusVM a;
    if (!build(&prog, &a)) return 0;

    int ok = exec_n(&a, 3);
    ok = ok && (zeus_vm_snapshot(&a, p1) == ZEUS_OK);

    FILE *f = fopen(p1, "r+b");
    ok = ok && f != NULL;
    if (f) {
        fseek(f, 120, SEEK_SET);
        int ch = fgetc(f);
        fseek(f, 120, SEEK_SET);
        fputc(ch ^ 0xFF, f);
        fclose(f);
    }

    ZeusVM b;
    zeus_vm_init(&b);
    ZeusError e = zeus_vm_restore(&b, p1);

    int rejected = (e == ZEUS_ERR_SNAPSHOT);
    zeus_vm_free(&b);

    zeus_vm_free(&a);
    zeus_program_free(&prog);
    remove(p1);
    return ok && rejected;
}

static int test_truncated_snapshot_rejected(void) {
    const char *p1 = "/tmp/zeus_snap_trunc_src.zsnap";
    const char *p2 = "/tmp/zeus_snap_trunc_dst.zsnap";

    ZeusProgram prog;
    ZeusVM a;
    if (!build(&prog, &a)) return 0;

    int ok = exec_n(&a, 3);
    ok = ok && (zeus_vm_snapshot(&a, p1) == ZEUS_OK);

    FILE *in = fopen(p1, "rb");
    FILE *out = fopen(p2, "wb");
    ok = ok && in && out;
    if (in && out) {
        uint8_t buf[100];
        size_t n = fread(buf, 1, sizeof(buf), in);
        fwrite(buf, 1, n, out);
    }
    if (in) fclose(in);
    if (out) fclose(out);

    ZeusVM b;
    zeus_vm_init(&b);
    ZeusError e = zeus_vm_restore(&b, p2);
    int rejected = (e == ZEUS_ERR_SNAPSHOT);
    zeus_vm_free(&b);

    zeus_vm_free(&a);
    zeus_program_free(&prog);
    remove(p1);
    remove(p2);
    return ok && rejected;
}

static int test_pending_effect_survives(void) {
    const char *p1 = "/tmp/zeus_snap_pending.zsnap";
    const char *src = "PUSH 65\nPRINTC\nHALT\n";

    ZeusProgram prog;
    ZeusVM a;
    char err[256];
    zeus_program_init(&prog);
    if (zeus_assemble(src, &prog, err, sizeof(err)) != ZEUS_OK) return 0;
    zeus_vm_init(&a);
    if (zeus_vm_load(&a, &prog) != ZEUS_OK) return 0;
    zeus_vm_start(&a);

    ZeusEffect eff;
    int ok = (zeus_vm_exec(&a, &eff) == ZEUS_OK);      /* PUSH 65 */
    ok = ok && (zeus_vm_exec(&a, &eff) == ZEUS_OK);    /* PRINTC -> yield */
    ok = ok && a.yielded;
    ok = ok && (a.pending.kind == ZEUS_EFFECT_PRINTC);
    ok = ok && (a.pending.value == 65);

    ok = ok && (zeus_vm_snapshot(&a, p1) == ZEUS_OK);

    ZeusVM b;
    ok = ok && restore_into(&b, p1);
    ok = ok && b.yielded;
    ok = ok && (b.pending.kind == ZEUS_EFFECT_PRINTC);
    ok = ok && (b.pending.value == 65);

    /* Delivering the restored effect pushes its result, then HALT runs. */
    ok = ok && (zeus_vm_resume(&b, 0) == ZEUS_OK);
    ok = ok && !b.yielded;
    ok = ok && run_to_end(&b);
    ok = ok && (b.error == ZEUS_OK);

    zeus_vm_free(&b);
    zeus_vm_free(&a);
    zeus_program_free(&prog);
    remove(p1);
    return ok;
}

static int test_restore_is_atomic(void) {
    const char *good = "/tmp/zeus_snap_atomic_good.zsnap";
    const char *bad = "/tmp/zeus_snap_atomic_bad.zsnap";

    ZeusProgram prog;
    ZeusVM a;
    if (!build(&prog, &a)) return 0;

    int ok = exec_n(&a, 3);
    ok = ok && (zeus_vm_snapshot(&a, good) == ZEUS_OK);

    Patch bad_pc[] = { { SNAP_OFF_PC, a.program.header.code_len + 1 } };
    ok = ok && patch_snapshot(good, bad, bad_pc, 1);

    /* A live machine with its own identity. */
    ZeusVM b;
    zeus_vm_init(&b);
    ok = ok && (zeus_vm_load(&b, &prog) == ZEUS_OK);
    zeus_vm_start(&b);
    ok = ok && exec_n(&b, 1);

    uint32_t pc_before = b.pc;
    int64_t sp_before = b.sp;
    uint8_t mem_before = b.memory[100];
    uint32_t code_len_before = b.program.header.code_len;

    ok = ok && (zeus_vm_restore(&b, bad) == ZEUS_ERR_SNAPSHOT);

    /* Failed restore must not disturb the machine it was given. */
    ok = ok && (b.pc == pc_before);
    ok = ok && (b.sp == sp_before);
    ok = ok && (b.memory[100] == mem_before);
    ok = ok && (b.program.header.code_len == code_len_before);
    ok = ok && b.running;
    ok = ok && run_to_end(&b);

    zeus_vm_free(&b);
    zeus_vm_free(&a);
    zeus_program_free(&prog);
    remove(good);
    remove(bad);
    return ok;
}

static int test_restore_rejects_inconsistent_state(void) {
    const char *p1 = "/tmp/zeus_snap_bad_state_src.zsnap";
    const char *p2 = "/tmp/zeus_snap_bad_state_dst.zsnap";

    ZeusProgram prog;
    ZeusVM a;
    if (!build(&prog, &a)) return 0;

    int ok = exec_n(&a, 3);
    ok = ok && (zeus_vm_snapshot(&a, p1) == ZEUS_OK);
    uint32_t code_len = a.program.header.code_len;

    /* Yielded but no effect staged. */
    Patch yielded_no_effect[] = { { SNAP_OFF_YIELDED, 1 } };
    ok = ok && rejected_after_patch(p1, p2, yielded_no_effect, 1);

    /* Running with a stale error. */
    Patch running_with_error[] = { { SNAP_OFF_ERROR, ZEUS_ERR_DIV_ZERO } };
    ok = ok && rejected_after_patch(p1, p2, running_with_error, 1);

    /* Program counter past the code section. */
    Patch bad_pc[] = { { SNAP_OFF_PC, code_len + 1 } };
    ok = ok && rejected_after_patch(p1, p2, bad_pc, 1);

    /* Pending send whose buffer runs past memory. */
    Patch bad_pending[] = {
        { SNAP_OFF_YIELDED, 1 },
        { SNAP_OFF_PENDING_KIND, ZEUS_EFFECT_SEND },
        { SNAP_OFF_PENDING_BUF, ZEUS_MEMORY_SIZE },
        { SNAP_OFF_PENDING_LEN, 8 }
    };
    ok = ok && rejected_after_patch(p1, p2, bad_pending, 4);

    zeus_vm_free(&a);
    zeus_program_free(&prog);
    remove(p1);
    remove(p2);
    return ok;
}

static int test_packet_slot_ids_survive(void) {
    const char *p1 = "/tmp/zeus_snap_packets.zsnap";

    ZeusProgram prog;
    ZeusVM a;
    if (!build(&prog, &a)) return 0;

    int ok = exec_n(&a, 3);
    ok = ok && (zeus_net_packet_alloc(a.net, 64) == 0);
    ok = ok && (zeus_net_packet_alloc(a.net, 64) == 1);
    ok = ok && (zeus_net_packet_alloc(a.net, 64) == 2);
    ok = ok && (zeus_net_packet_free(a.net, 1) == ZEUS_OK);
    ok = ok && (zeus_net_packet_set_proto(a.net, 2, 17) == ZEUS_OK);
    ok = ok && (zeus_net_packet_set_dst(a.net, 2, 0x01020304u) == ZEUS_OK);
    ok = ok && (zeus_vm_snapshot(&a, p1) == ZEUS_OK);

    ZeusVM b;
    ok = ok && restore_into(&b, p1);
    ok = ok && b.net;
    ok = ok && b.net->packets[0].used;
    ok = ok && !b.net->packets[1].used;
    ok = ok && b.net->packets[2].used;
    ok = ok && (b.net->packets[2].proto == 17);
    ok = ok && (b.net->packets[2].dst == 0x01020304u);
    ok = ok && (b.net->packets[2].size == 64);
    ok = ok && (zeus_net_packet_set_proto(b.net, 2, 6) == ZEUS_OK);
    ok = ok && (zeus_net_packet_set_proto(b.net, 1, 6) == ZEUS_ERR_PACKET);

    zeus_vm_free(&b);
    zeus_vm_free(&a);
    zeus_program_free(&prog);
    remove(p1);
    return ok;
}

static int test_v2_snapshot_rejected(void) {
    const char *src = "/tmp/zeus_snap_v2_src.zsnap";
    const char *dst = "/tmp/zeus_snap_v2_dst.zsnap";

    ZeusProgram prog;
    ZeusVM a;
    if (!build(&prog, &a)) return 0;

    int ok = exec_n(&a, 3);
    ok = ok && (zeus_vm_snapshot(&a, src) == ZEUS_OK);

    /* Old magic with a matching checksum: the version, not corruption, is
     * what must justify the refusal. */
    ok = ok && patch_snapshot_byte(src, dst, 5, '2');
    ok = ok && rejects_file(dst);

    /* New magic but a header that still says format 2. */
    Patch old_format[] = { { SNAP_OFF_FORMAT, 2 } };
    ok = ok && rejected_after_patch(src, dst, old_format, 1);

    zeus_vm_free(&a);
    zeus_program_free(&prog);
    remove(src);
    remove(dst);
    return ok;
}

static int test_bound_snapshot_roundtrip(void) {
    const char *p1 = "/tmp/zeus_snap_bound_a.zsnap";
    const char *p2 = "/tmp/zeus_snap_bound_b.zsnap";
    const char *lineage = "0123456789abcdef0123456789abcdef";

    ZeusProgram prog;
    ZeusVM a;
    if (!build(&prog, &a)) return 0;

    int ok = exec_n(&a, 3);
    ok = ok && (zeus_vm_snapshot_bound(&a, p1, lineage, 7) == ZEUS_OK);

    ZeusVM b;
    zeus_vm_init(&b);
    ZeusSnapshotMeta meta;
    memset(&meta, 0, sizeof(meta));
    ok = ok && (zeus_vm_restore_ex(&b, p1, &meta) == ZEUS_OK);
    ok = ok && (strcmp(meta.lineage, lineage) == 0);
    ok = ok && (meta.seq == 7);
    ok = ok && (meta.journal_head == 0);

    /* Writing the restored state with the same binding is byte-identical: the
     * metadata is part of the deterministic payload. */
    ok = ok && (zeus_vm_snapshot_bound(&b, p2, lineage, 7) == ZEUS_OK);
    ok = ok && files_identical(p1, p2);

    ok = ok && run_to_end(&b);
    ok = ok && (b.error == ZEUS_OK);
    ok = ok && (b.memory[100] == 7);

    /* A malformed lineage is refused before anything is written, and a
     * too-long id is not truncated into a different valid one. */
    ok = ok && (zeus_vm_snapshot_bound(&a, p2, "not-a-lineage", 1)
                == ZEUS_ERR_SNAPSHOT);
    ok = ok && (zeus_vm_snapshot_bound(
                    &a, p2, "0123456789abcdef0123456789abcdef01234567", 1)
                == ZEUS_ERR_SNAPSHOT);

    zeus_vm_free(&b);
    zeus_vm_free(&a);
    zeus_program_free(&prog);
    remove(p1);
    remove(p2);
    return ok;
}

static int test_unbound_snapshot_roundtrip(void) {
    const char *p1 = "/tmp/zeus_snap_unbound.zsnap";

    ZeusProgram prog;
    ZeusVM a;
    if (!build(&prog, &a)) return 0;

    int ok = exec_n(&a, 3);
    ok = ok && (zeus_vm_snapshot(&a, p1) == ZEUS_OK);

    ZeusVM b;
    zeus_vm_init(&b);
    ZeusSnapshotMeta meta;
    memset(&meta, 0xff, sizeof(meta));
    ok = ok && (zeus_vm_restore_ex(&b, p1, &meta) == ZEUS_OK);
    ok = ok && (meta.lineage[0] == '\0');
    ok = ok && (meta.seq == 0);
    ok = ok && (meta.journal_head == 0);

    ok = ok && run_to_end(&b);
    ok = ok && (b.memory[100] == 7);

    zeus_vm_free(&b);
    zeus_vm_free(&a);
    zeus_program_free(&prog);
    remove(p1);
    return ok;
}

static int test_snapshot_write_is_atomic(void) {
    char dir[] = "/tmp/zeus_snap_atomicXXXXXX";
    if (!mkdtemp(dir)) return 0;

    char good[600], backup[600];
    snprintf(good, sizeof(good), "%s/good.zsnap", dir);
    snprintf(backup, sizeof(backup), "%s/good.backup", dir);

    ZeusProgram prog;
    ZeusVM a;
    if (!build(&prog, &a)) { rmdir(dir); return 0; }

    int ok = exec_n(&a, 3);
    ok = ok && (zeus_vm_snapshot(&a, good) == ZEUS_OK);
    ok = ok && copy_file(good, backup);

    /* A directory without write permission refuses the temporary file before
     * the destination is touched. */
    ok = ok && (chmod(dir, 0500) == 0);
    ZeusError e = zeus_vm_snapshot(&a, good);
    ok = ok && (chmod(dir, 0700) == 0);

    int refused = (e != ZEUS_OK);
    if (!refused && geteuid() == 0) {
        /* root bypasses directory permissions: force the write itself to fail
         * so the same property (old file intact, temp removed) is proven. */
        struct rlimit old, rl;
        ok = ok && (getrlimit(RLIMIT_FSIZE, &old) == 0);
        rl = old;
        rl.rlim_cur = 1024;

        struct sigaction ign, prev;
        memset(&ign, 0, sizeof(ign));
        ign.sa_handler = SIG_IGN;
        sigemptyset(&ign.sa_mask);
        ok = ok && (sigaction(SIGXFSZ, &ign, &prev) == 0);
        ok = ok && (setrlimit(RLIMIT_FSIZE, &rl) == 0);

        e = zeus_vm_snapshot(&a, good);

        (void)setrlimit(RLIMIT_FSIZE, &old);
        (void)sigaction(SIGXFSZ, &prev, NULL);
        refused = (e != ZEUS_OK);
    }

    ok = ok && refused;
    ok = ok && files_identical(good, backup);
    ok = ok && !dir_has_temp(dir);

    zeus_vm_free(&a);
    zeus_program_free(&prog);
    remove(good);
    remove(backup);
    rmdir(dir);
    return ok;
}

static int test_snapshot_rejects_inconsistent_machine(void) {
    const char *p1 = "/tmp/zeus_snap_bad_machine.zsnap";

    ZeusProgram prog;
    ZeusVM a;
    if (!build(&prog, &a)) return 0;

    int ok = exec_n(&a, 3);

    a.yielded = true;                       /* yield with no effect staged */
    ok = ok && (zeus_vm_snapshot(&a, p1) == ZEUS_ERR_SNAPSHOT);

    a.yielded = false;
    memset(&a.pending, 0, sizeof(a.pending));
    a.pending.value = 7;                    /* effect data without a yield */
    ok = ok && (zeus_vm_snapshot(&a, p1) == ZEUS_ERR_SNAPSHOT);

    memset(&a.pending, 0, sizeof(a.pending));
    ok = ok && (zeus_vm_snapshot(&a, p1) == ZEUS_OK);

    zeus_vm_free(&a);
    zeus_program_free(&prog);
    remove(p1);
    return ok;
}

int main(void) {
    printf("Running snapshot tests...\n");
    RUN_TEST(test_roundtrip_resumes_exactly);
    RUN_TEST(test_snapshot_is_byte_stable);
    RUN_TEST(test_fork_gives_independent_machines);
    RUN_TEST(test_tampered_snapshot_rejected);
    RUN_TEST(test_truncated_snapshot_rejected);
    RUN_TEST(test_pending_effect_survives);
    RUN_TEST(test_restore_is_atomic);
    RUN_TEST(test_restore_rejects_inconsistent_state);
    RUN_TEST(test_packet_slot_ids_survive);
    RUN_TEST(test_snapshot_rejects_inconsistent_machine);
    RUN_TEST(test_v2_snapshot_rejected);
    RUN_TEST(test_bound_snapshot_roundtrip);
    RUN_TEST(test_unbound_snapshot_roundtrip);
    RUN_TEST(test_snapshot_write_is_atomic);

    printf("\n%d/%d tests passed\n", tests_run - tests_failed, tests_run);
    return tests_failed > 0 ? 1 : 0;
}
