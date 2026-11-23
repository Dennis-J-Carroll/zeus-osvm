#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include "zeus.h"
#include "vm.h"
#include "asm.h"
#include "snapshot.h"
#include "policy.h"
#include "supervisor.h"

#define GUARD_LINEAGE "0123456789abcdef0123456789abcdef"

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

static int build(ZeusProgram *prog, ZeusVM *vm, const char *src) {
    char err[256];
    zeus_program_init(prog);
    if (zeus_assemble(src, prog, err, sizeof(err)) != ZEUS_OK) {
        fprintf(stderr, "asm error: %s\n", err);
        return 0;
    }
    zeus_vm_init(vm);
    if (zeus_vm_load(vm, prog) != ZEUS_OK) return 0;
    return 1;
}

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

static int count_substr(const char *hay, const char *needle) {
    int n = 0;
    size_t len = strlen(needle);
    for (const char *p = hay; p && (p = strstr(p, needle)) != NULL; p += len) n++;
    return n;
}

static int append_line(const char *path, const char *line) {
    FILE *f = fopen(path, "a");
    if (!f) return 0;
    int ok = fprintf(f, "%s\n", line) > 0;
    if (fclose(f) != 0) ok = 0;
    return ok;
}

static int write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t len = strlen(text);
    int ok = fwrite(text, 1, len, f) == len;
    if (fclose(f) != 0) ok = 0;
    return ok;
}

/* ------------------------------------------------------------------ */

static int test_policy_defaults_to_deny(void) {
    ZeusPolicy p;
    ZeusEffect print = { .kind = ZEUS_EFFECT_PRINT, .value = 1 };

    zeus_policy_init(&p, ZEUS_PROFILE_DENY_ALL);
    if (zeus_policy_check(&p, &print) != ZEUS_POLICY_DENY) return 0;
    if (strcmp(zeus_policy_reason(&p, &print), "kind_not_allowed") != 0) return 0;

    zeus_policy_init(&p, ZEUS_PROFILE_STANDARD);
    if (zeus_policy_check(&p, &print) != ZEUS_POLICY_ALLOW) return 0;

    ZeusEffect raw = { .kind = ZEUS_EFFECT_RAW_ALLOC, .value = 64 };
    if (zeus_policy_check(&p, &raw) != ZEUS_POLICY_DENY) return 0;
    if (strcmp(zeus_policy_reason(&p, &raw), "raw_capability_disabled") != 0) return 0;

    /* The raw master switch wins even when a kind was allowed by hand. */
    zeus_policy_allow(&p, ZEUS_EFFECT_RAW_ALLOC);
    if (zeus_policy_check(&p, &raw) != ZEUS_POLICY_DENY) return 0;

    zeus_policy_init(&p, ZEUS_PROFILE_FULL);
    if (zeus_policy_check(&p, &raw) != ZEUS_POLICY_ALLOW) return 0;

    return 1;
}

static int test_denied_effect_kills_cleanly(void) {
    const char *snap = "/tmp/zeus_guard_denied.zsnap";
    remove(snap);

    ZeusProgram prog;
    ZeusVM vm;
    if (!build(&prog, &vm, "PUSH 1\nPRINT\nHALT\n")) return 0;

    ZeusPolicy p;
    zeus_policy_init(&p, ZEUS_PROFILE_DENY_ALL);

    ZeusSupervisor s;
    if (!zeus_supervisor_open(&s, &p, NULL, NULL)) return 0;

    char why[256];
    ZeusDriveResult r = zeus_supervisor_drive(&s, &vm, -1, why, sizeof(why));

    int ok = (r == ZEUS_DRIVE_DENIED);
    ok = ok && (vm.error == ZEUS_ERR_EFFECT);
    ok = ok && !vm.running && !vm.yielded;

    /* The old failure path left yielded=true; this state must serialize. */
    ok = ok && (zeus_vm_snapshot(&vm, snap) == ZEUS_OK);
    ok = ok && (strstr(why, "policy denied") != NULL);

    zeus_supervisor_close(&s);
    zeus_vm_free(&vm);
    zeus_program_free(&prog);
    remove(snap);
    return ok;
}

static int test_journal_is_append_only(void) {
    const char *j = "/tmp/zeus_guard_journal.log";
    remove(j);

    ZeusPolicy p;
    zeus_policy_init(&p, ZEUS_PROFILE_STANDARD);

    ZeusProgram prog;
    ZeusVM vm;
    if (!build(&prog, &vm, "PUSH 7\nPRINT\nHALT\n")) return 0;

    ZeusSupervisor s;
    if (!zeus_supervisor_open(&s, &p, j, NULL)) return 0;

    char why[256];
    int ok = (zeus_supervisor_drive(&s, &vm, -1, why, sizeof(why))
              == ZEUS_DRIVE_HALTED);
    ok = ok && (s.seq == 1);
    zeus_supervisor_close(&s);

    char *first = slurp(j);
    ok = ok && first != NULL;
    ok = ok && (count_substr(first, "\"event\":\"intent\"") == 1);
    ok = ok && (count_substr(first, "\"event\":\"commit\"") == 1);

    if (ok) {
        char *i1 = strstr(first, "\"event\":\"intent\"");
        char *c1 = strstr(first, "\"event\":\"commit\"");
        ok = i1 && c1 && (i1 < c1);
    }

    /* A second supervisor appends; nothing from the first run is lost. */
    ZeusProgram prog2;
    ZeusVM vm2;
    if (!build(&prog2, &vm2, "PUSH 8\nPRINT\nHALT\n")) {
        free(first);
        return 0;
    }

    ZeusSupervisor s2;
    ok = ok && zeus_supervisor_open(&s2, &p, j, NULL);
    ok = ok && (zeus_supervisor_drive(&s2, &vm2, -1, why, sizeof(why))
                == ZEUS_DRIVE_HALTED);
    zeus_supervisor_close(&s2);

    char *second = slurp(j);
    ok = ok && second && first;
    if (ok) {
        ok = (strlen(second) > strlen(first));
        ok = ok && (strncmp(second, first, strlen(first)) == 0);
        ok = ok && (count_substr(second, "\"event\":\"start\"") == 2);
    }

    free(second);
    free(first);
    zeus_vm_free(&vm2);
    zeus_program_free(&prog2);
    zeus_vm_free(&vm);
    zeus_program_free(&prog);
    remove(j);
    return ok;
}

static int test_sequence_survives_restore(void) {
    const char *j = "/tmp/zeus_guard_seq.log";
    const char *snap = "/tmp/zeus_guard_seq.zsnap";
    remove(j);
    remove(snap);

    ZeusPolicy p;
    zeus_policy_init(&p, ZEUS_PROFILE_STANDARD);

    ZeusProgram prog;
    ZeusVM vm;
    if (!build(&prog, &vm,
               "PUSH 1\nPRINT\nPUSH 2\nPRINT\nPUSH 3\nPRINT\nHALT\n")) {
        return 0;
    }

    ZeusSupervisor s;
    if (!zeus_supervisor_open(&s, &p, j, NULL)) return 0;

    char lineage[ZEUS_ID_LEN];
    snprintf(lineage, sizeof(lineage), "%s", s.lineage_id);

    char why[256];
    int ok = (zeus_supervisor_drive(&s, &vm, 3, why, sizeof(why))
              == ZEUS_DRIVE_STEP_LIMIT);
    ok = ok && (s.seq == 1);
    ok = ok && (zeus_vm_snapshot(&vm, snap) == ZEUS_OK);
    zeus_supervisor_close(&s);

    /* The counter lives in the journal: a restored machine cannot rewind it. */
    ZeusSupervisor s2;
    ok = ok && zeus_supervisor_open(&s2, &p, j, lineage);
    ok = ok && (s2.seq == 1);
    ok = ok && !s2.ambiguous;

    ZeusVM vm2;
    zeus_vm_init(&vm2);
    ok = ok && (zeus_vm_restore(&vm2, snap) == ZEUS_OK);
    ok = ok && (zeus_supervisor_drive(&s2, &vm2, -1, why, sizeof(why))
                == ZEUS_DRIVE_HALTED);
    ok = ok && (s2.seq == 3);
    zeus_supervisor_close(&s2);

    char *txt = slurp(j);
    ok = ok && txt && (count_substr(txt, "\"event\":\"intent\"") == 3);

    free(txt);
    zeus_vm_free(&vm2);
    zeus_vm_free(&vm);
    zeus_program_free(&prog);
    remove(j);
    remove(snap);
    return ok;
}

static int test_approval_binds_the_held_machine(void) {
    const char *j = "/tmp/zeus_guard_ask.log";
    const char *snap = "/tmp/zeus_guard_ask.zsnap";
    const char *dossier = "/tmp/zeus_guard_ask.zsnap.dossier";
    const char *appr = "/tmp/zeus_guard_ask.approve";
    const char *bad = "/tmp/zeus_guard_ask_bad.approve";
    remove(j);
    remove(snap);
    remove(dossier);
    remove(appr);
    remove(bad);

    ZeusPolicy p;
    zeus_policy_init(&p, ZEUS_PROFILE_STANDARD);
    zeus_policy_ask(&p, ZEUS_EFFECT_PRINTC);

    ZeusProgram prog;
    ZeusVM vm;
    if (!build(&prog, &vm, "PUSH 65\nPRINTC\nHALT\n")) return 0;

    ZeusSupervisor s;
    if (!zeus_supervisor_open(&s, &p, j, NULL)) return 0;
    s.ask = true;
    zeus_supervisor_set_paths(&s, snap);

    char why[256];
    int ok = (zeus_supervisor_drive(&s, &vm, -1, why, sizeof(why))
              == ZEUS_DRIVE_NEEDS_APPROVAL);
    ok = ok && vm.yielded && (vm.pending.kind == ZEUS_EFFECT_PRINTC);
    ok = ok && (vm.pending.value == 65);
    ok = ok && (s.seq == 0);          /* nothing was delivered */
    zeus_supervisor_close(&s);

    ZeusDossier d;
    ok = ok && zeus_dossier_load(dossier, &d);
    ok = ok && (d.seq == 1);
    ok = ok && (d.digest == zeus_effect_digest(&vm, &vm.pending));
    ok = ok && (strcmp(d.effect, "printc") == 0);

    ok = ok && zeus_approval_write(&d, appr);

    ZeusSupervisor s2;
    ok = ok && zeus_supervisor_open(&s2, &p, j, d.lineage);

    ZeusApproval a;
    ok = ok && zeus_approval_load(appr, &a);
    ok = ok && zeus_approval_binds(&a, &s2, &vm, &vm.pending);

    /* A different held machine must not match the same approval. */
    ZeusVM other;
    zeus_vm_init(&other);
    ok = ok && (zeus_vm_restore(&other, snap) == ZEUS_OK);
    other.pending.value = 66;
    ok = ok && !zeus_approval_binds(&a, &s2, &other, &other.pending);
    zeus_vm_free(&other);

    /* Same lineage/seq with the wrong digest is not an approval. */
    ZeusApproval wrong;
    FILE *f = fopen(bad, "w");
    ok = ok && f != NULL;
    if (f) {
        fprintf(f, "format=1\nkind=approval\nlineage=%s\nseq=1\n"
                   "digest=0x0000000000000000\n", d.lineage);
        fclose(f);
        ok = ok && zeus_approval_load(bad, &wrong);
        ok = ok && !zeus_approval_binds(&wrong, &s2, &vm, &vm.pending);
    }

    ZeusVM resumed;
    zeus_vm_init(&resumed);
    ok = ok && (zeus_vm_restore(&resumed, snap) == ZEUS_OK);
    s2.ask = true;
    zeus_supervisor_set_paths(&s2, snap);
    zeus_supervisor_bind_approval(&s2, &a);
    ok = ok && (zeus_supervisor_drive(&s2, &resumed, -1, why, sizeof(why))
                == ZEUS_DRIVE_HALTED);
    ok = ok && (resumed.error == ZEUS_OK) && !resumed.yielded;
    zeus_supervisor_close(&s2);

    zeus_vm_free(&resumed);
    zeus_vm_free(&vm);
    zeus_program_free(&prog);
    remove(j);
    remove(snap);
    remove(dossier);
    remove(appr);
    remove(bad);
    return ok;
}

static int test_denial_spends_its_sequence(void) {
    const char *j = "/tmp/zeus_guard_denyseq.log";
    const char *snap = "/tmp/zeus_guard_denyseq.zsnap";
    const char *dossier = "/tmp/zeus_guard_denyseq.zsnap.dossier";
    remove(j);
    remove(snap);
    remove(dossier);

    ZeusPolicy p;
    zeus_policy_init(&p, ZEUS_PROFILE_STANDARD);
    zeus_policy_ask_all(&p, false);

    ZeusProgram prog;
    ZeusVM vm;
    if (!build(&prog, &vm, "READ\nHALT\n")) return 0;

    ZeusSupervisor s;
    if (!zeus_supervisor_open(&s, &p, j, NULL)) return 0;
    s.ask = true;
    zeus_supervisor_set_paths(&s, snap);

    char why[256];
    int ok = (zeus_supervisor_drive(&s, &vm, -1, why, sizeof(why))
              == ZEUS_DRIVE_NEEDS_APPROVAL);
    uint64_t pending = zeus_supervisor_next_seq(&s);
    ok = ok && (pending == 1) && vm.yielded;

    ok = ok && zeus_supervisor_deny(&s, &vm, "operator_denied");
    /* The refusal is an outcome: its number is spent, so an approval naming
     * it can never authorise a later effect under the same seq. */
    ok = ok && (s.seq == pending);
    ok = ok && (zeus_supervisor_next_seq(&s) == pending + 1);
    ok = ok && !vm.yielded && (vm.error == ZEUS_ERR_EFFECT);

    char lineage[ZEUS_ID_LEN];
    snprintf(lineage, sizeof(lineage), "%s", s.lineage_id);
    zeus_supervisor_close(&s);

    /* Reopening the lineage agrees: the counter lives in the journal. */
    ZeusSupervisor s2;
    ok = ok && zeus_supervisor_open(&s2, &p, j, lineage);
    ok = ok && (s2.seq == pending) && !s2.ambiguous;
    zeus_supervisor_close(&s2);

    char *txt = slurp(j);
    ok = ok && txt && (count_substr(txt, "\"event\":\"deny\"") == 1);
    free(txt);

    zeus_vm_free(&vm);
    zeus_program_free(&prog);
    remove(j);
    remove(snap);
    remove(dossier);
    return ok;
}

static int test_uncommitted_intent_refuses(void) {
    const char *j = "/tmp/zeus_guard_amb.log";
    remove(j);

    ZeusSupervisor seed;
    zeus_supervisor_init(&seed);
    char lineage[ZEUS_ID_LEN];
    snprintf(lineage, sizeof(lineage), "%s", seed.lineage_id);

    char line[512];
    snprintf(line, sizeof(line),
             "{\"seq\":0,\"event\":\"start\",\"machine\":\"%s\",\"lineage\":\"%s\"}",
             seed.machine_id, lineage);
    int ok = append_line(j, line);
    snprintf(line, sizeof(line),
             "{\"seq\":1,\"event\":\"intent\",\"machine\":\"%s\","
             "\"lineage\":\"%s\",\"effect\":\"send\"}", seed.machine_id, lineage);
    ok = ok && append_line(j, line);
    /* An older build appended a start record after the unmatched intent.
     * The scan must look through it; otherwise that journal now looks clean. */
    snprintf(line, sizeof(line),
             "{\"seq\":0,\"event\":\"start\",\"machine\":\"%s\",\"lineage\":\"%s\"}",
             seed.machine_id, lineage);
    ok = ok && append_line(j, line);

    ZeusPolicy p;
    zeus_policy_init(&p, ZEUS_PROFILE_STANDARD);

    ZeusSupervisor s;
    ok = ok && zeus_supervisor_open(&s, &p, j, lineage);
    ok = ok && s.ambiguous && (s.ambiguous_seq == 1);

    ZeusProgram prog;
    ZeusVM vm;
    if (!build(&prog, &vm, "PUSH 1\nPRINT\nHALT\n")) {
        zeus_supervisor_close(&s);
        remove(j);
        return 0;
    }

    char why[256];
    ok = ok && (zeus_supervisor_drive(&s, &vm, -1, why, sizeof(why))
                == ZEUS_DRIVE_FAILED);
    ok = ok && (strstr(why, "uncommitted") != NULL);
    ok = ok && (vm.pc == 0) && !vm.started;   /* nothing was executed */
    zeus_supervisor_close(&s);

    /* A commit record for that seq makes the lineage clean again. */
    snprintf(line, sizeof(line),
             "{\"seq\":1,\"event\":\"commit\",\"machine\":\"%s\","
             "\"lineage\":\"%s\",\"result\":0,\"error\":0}", seed.machine_id, lineage);
    ok = ok && append_line(j, line);

    ZeusSupervisor s2;
    ok = ok && zeus_supervisor_open(&s2, &p, j, lineage);
    ok = ok && !s2.ambiguous && (s2.seq == 1);
    zeus_supervisor_close(&s2);

    zeus_vm_free(&vm);
    zeus_program_free(&prog);
    remove(j);
    return ok;
}

/* The refused open must not repair the journal: it used to append a `start`
 * record after the unmatched intent, so the second attempt saw a clean
 * lineage and replayed an effect that may already have happened. Two retries
 * in a row must both refuse, and neither may write to the journal. */
static int test_ambiguity_survives_retries(void) {
    const char *j = "/tmp/zeus_guard_amb_retry.log";
    remove(j);

    ZeusSupervisor seed;
    zeus_supervisor_init(&seed);
    char lineage[ZEUS_ID_LEN];
    snprintf(lineage, sizeof(lineage), "%s", seed.lineage_id);

    char line[512];
    snprintf(line, sizeof(line),
             "{\"seq\":0,\"event\":\"start\",\"machine\":\"%s\",\"lineage\":\"%s\"}",
             seed.machine_id, lineage);
    int ok = append_line(j, line);
    snprintf(line, sizeof(line),
             "{\"seq\":1,\"event\":\"intent\",\"machine\":\"%s\","
             "\"lineage\":\"%s\",\"effect\":\"send\"}", seed.machine_id, lineage);
    ok = ok && append_line(j, line);

    ZeusPolicy p;
    zeus_policy_init(&p, ZEUS_PROFILE_STANDARD);

    ZeusProgram prog;
    ZeusVM vm;
    if (!build(&prog, &vm, "PUSH 1\nPRINT\nHALT\n")) {
        remove(j);
        return 0;
    }

    for (int attempt = 0; attempt < 2; attempt++) {
        ZeusSupervisor s;
        ok = ok && zeus_supervisor_open(&s, &p, j, lineage);
        ok = ok && s.ambiguous && (s.ambiguous_seq == 1);

        char why[256];
        ok = ok && (zeus_supervisor_drive(&s, &vm, -1, why, sizeof(why))
                    == ZEUS_DRIVE_FAILED);
        ok = ok && (strstr(why, "uncommitted") != NULL);
        ok = ok && !vm.started && (vm.pc == 0);
        zeus_supervisor_close(&s);
    }

    /* Exactly the seeded start record: neither retry wrote anything. */
    char *txt = slurp(j);
    ok = ok && txt && (count_substr(txt, "\"event\":\"start\"") == 1);
    free(txt);

    zeus_vm_free(&vm);
    zeus_program_free(&prog);
    remove(j);
    return ok;
}

/* Two supervisors that both scanned the same journal would mint the same
 * sequence number. The journal is locked before the scan, so the second open
 * fails and writes nothing. */
static int test_journal_lock_is_exclusive(void) {
    const char *j = "/tmp/zeus_guard_lock.log";
    remove(j);

    int fd = open(j, O_RDWR | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return 0;
    int ok = (flock(fd, LOCK_EX | LOCK_NB) == 0);

    ZeusPolicy p;
    zeus_policy_init(&p, ZEUS_PROFILE_STANDARD);

    ZeusSupervisor s;
    ok = ok && !zeus_supervisor_open(&s, &p, j, NULL);
    ok = ok && s.journal_busy;

    /* The refused open must not have appended a start record. */
    char *txt = slurp(j);
    ok = ok && txt && (count_substr(txt, "\"event\":\"start\"") == 0);
    free(txt);

    ok = ok && (flock(fd, LOCK_UN) == 0);
    ok = ok && (close(fd) == 0);

    if (ok) {
        ZeusSupervisor s2;
        ok = zeus_supervisor_open(&s2, &p, j, NULL) && !s2.journal_busy;
        if (ok) {
            txt = slurp(j);
            ok = txt && (count_substr(txt, "\"event\":\"start\"") == 1);
            free(txt);
            zeus_supervisor_close(&s2);
        }
    }

    remove(j);
    return ok;
}

/* A dossier is a human decision document. Anything ambiguous in it (a
 * truncated value, a duplicate key, an oversized field) must fail the load,
 * not produce a plausible-looking approval. */
static int test_dossier_rejects_hostile_input(void) {
    const char *p = "/tmp/zeus_guard_hostile.dossier";
    ZeusDossier d;

    const char *good =
        "format=1\n"
        "kind=dossier\n"
        "lineage=" GUARD_LINEAGE "\n"
        "seq=7\n"
        "effect=read\n"
        "digest=0x0123456789abcdef\n"
        "snapshot=/tmp/m.zsnap\n";

    int ok = write_text(p, good);
    ok = ok && zeus_dossier_load(p, &d);
    ok = ok && (d.seq == 7) && (d.digest == UINT64_C(0x0123456789abcdef));
    ok = ok && (strcmp(d.lineage, GUARD_LINEAGE) == 0);
    ok = ok && (strcmp(d.snapshot, "/tmp/m.zsnap") == 0);

    /* Truncated mid-value: no final newline, so the digest is incomplete. */
    ok = ok && write_text(p,
        "format=1\nkind=dossier\nlineage=" GUARD_LINEAGE
        "\nseq=7\ndigest=0x0123456789abcd");
    ok = ok && !zeus_dossier_load(p, &d);

    /* Truncated at a line boundary: a required field is missing. */
    ok = ok && write_text(p,
        "format=1\nkind=dossier\nlineage=" GUARD_LINEAGE "\nseq=7\n");
    ok = ok && !zeus_dossier_load(p, &d);

    /* Duplicate keys are ambiguous even when the values agree. */
    ok = ok && write_text(p,
        "format=1\nkind=dossier\nlineage=" GUARD_LINEAGE
        "\nseq=7\ndigest=0x1\nseq=7\n");
    ok = ok && !zeus_dossier_load(p, &d);
    ok = ok && write_text(p,
        "format=1\nkind=dossier\nlineage=" GUARD_LINEAGE
        "\nseq=7\ndigest=0x1\nseq=8\n");
    ok = ok && !zeus_dossier_load(p, &d);

    /* A value that fits the line buffer but not the field. */
    ok = ok && write_text(p,
        "format=1\nkind=dossier\nlineage=" GUARD_LINEAGE "\nseq=7\n");
    FILE *f = fopen(p, "a");
    ok = ok && f != NULL;
    if (f) {
        fputs("digest=", f);
        for (int i = 0; i < ZEUS_PATH_LEN + 16; i++) fputc('a', f);
        fputc('\n', f);
        ok = ok && fclose(f) == 0;
    }
    ok = ok && !zeus_dossier_load(p, &d);

    /* A line longer than the parser's buffer. */
    ok = ok && write_text(p,
        "format=1\nkind=dossier\nlineage=" GUARD_LINEAGE "\nseq=7\n");
    f = fopen(p, "a");
    ok = ok && f != NULL;
    if (f) {
        fputs("digest=", f);
        for (int i = 0; i < 4096; i++) fputc('a', f);
        fputc('\n', f);
        ok = ok && fclose(f) == 0;
    }
    ok = ok && !zeus_dossier_load(p, &d);

    /* Trailing junk after a number is not the number. */
    ok = ok && write_text(p,
        "format=1\nkind=dossier\nlineage=" GUARD_LINEAGE
        "\nseq=7x\ndigest=0x1\n");
    ok = ok && !zeus_dossier_load(p, &d);

    /* Empty, wrong kind, short id, junk digest. */
    ok = ok && write_text(p, "");
    ok = ok && !zeus_dossier_load(p, &d);
    ok = ok && write_text(p,
        "format=1\nkind=approval\nlineage=" GUARD_LINEAGE
        "\nseq=7\ndigest=0x1\n");
    ok = ok && !zeus_dossier_load(p, &d);
    ok = ok && write_text(p,
        "format=1\nkind=dossier\nlineage=abc\nseq=7\ndigest=0x1\n");
    ok = ok && !zeus_dossier_load(p, &d);
    ok = ok && write_text(p,
        "format=1\nkind=dossier\nlineage=" GUARD_LINEAGE
        "\nseq=7\ndigest=0xzz\n");
    ok = ok && !zeus_dossier_load(p, &d);

    remove(p);
    return ok;
}

/* An approval is the last file between a parked effect and the host. A
 * hostile or damaged one must never load as a decision. */
static int test_approval_rejects_hostile_input(void) {
    const char *dossier = "/tmp/zeus_guard_hostile.dossier";
    const char *appr = "/tmp/zeus_guard_hostile.approve";
    const char *bad = "/tmp/zeus_guard_hostile_bad.approve";

    ZeusDossier d;
    int ok = write_text(dossier,
        "format=1\nkind=dossier\nlineage=" GUARD_LINEAGE
        "\nseq=7\ndigest=0x0123456789abcdef\nsnapshot=/tmp/m.zsnap\n");
    ok = ok && zeus_dossier_load(dossier, &d);
    ok = ok && zeus_approval_write(&d, appr);

    /* The writer's own output round-trips. */
    ZeusApproval a;
    ok = ok && zeus_approval_load(appr, &a);
    ok = ok && (a.seq == 7) && (a.digest == UINT64_C(0x0123456789abcdef));
    ok = ok && (strcmp(a.lineage, GUARD_LINEAGE) == 0);

    /* Truncated: every field is present but the file has no final newline.
     * This used to load as a valid approval. */
    ok = ok && write_text(bad,
        "format=1\nkind=approval\nlineage=" GUARD_LINEAGE
        "\nseq=7\ndigest=0x0123456789abcdef");
    ok = ok && !zeus_approval_load(bad, &a);

    /* Truncated mid-digest: "0x0123" is a valid number, not a valid file. */
    ok = ok && write_text(bad,
        "format=1\nkind=approval\nlineage=" GUARD_LINEAGE
        "\nseq=7\ndigest=0x0123");
    ok = ok && !zeus_approval_load(bad, &a);

    /* Duplicate keys, agreeing or not, are ambiguous. */
    ok = ok && write_text(bad,
        "format=1\nkind=approval\nlineage=" GUARD_LINEAGE
        "\nseq=7\ndigest=0x1\nseq=7\n");
    ok = ok && !zeus_approval_load(bad, &a);
    ok = ok && write_text(bad,
        "format=1\nkind=approval\nlineage=" GUARD_LINEAGE
        "\nseq=7\ndigest=0x1\ndigest=0x2\n");
    ok = ok && !zeus_approval_load(bad, &a);

    /* Oversized values are refused, not silently truncated into a decision. */
    ok = ok && write_text(bad,
        "format=1\nkind=approval\nlineage=" GUARD_LINEAGE "\nseq=7\n");
    FILE *f = fopen(bad, "a");
    ok = ok && f != NULL;
    if (f) {
        fputs("digest=", f);
        for (int i = 0; i < ZEUS_PATH_LEN + 16; i++) fputc('a', f);
        fputc('\n', f);
        ok = ok && fclose(f) == 0;
    }
    ok = ok && !zeus_approval_load(bad, &a);

    /* Trailing junk after a number used to parse as the number. */
    ok = ok && write_text(bad,
        "format=1\nkind=approval\nlineage=" GUARD_LINEAGE
        "\nseq=7x\ndigest=0x1\n");
    ok = ok && !zeus_approval_load(bad, &a);

    /* Whitespace or a sign is not bare hex. */
    ok = ok && write_text(bad,
        "format=1\nkind=approval\nlineage=" GUARD_LINEAGE
        "\nseq=7\ndigest= 0x1\n");
    ok = ok && !zeus_approval_load(bad, &a);
    ok = ok && write_text(bad,
        "format=1\nkind=approval\nlineage=" GUARD_LINEAGE
        "\nseq=7\ndigest=+1\n");
    ok = ok && !zeus_approval_load(bad, &a);

    remove(bad);
    remove(appr);
    remove(dossier);
    return ok;
}

int main(void) {
    printf("Running protection tests...\n");
    RUN_TEST(test_policy_defaults_to_deny);
    RUN_TEST(test_denied_effect_kills_cleanly);
    RUN_TEST(test_journal_is_append_only);
    RUN_TEST(test_sequence_survives_restore);
    RUN_TEST(test_approval_binds_the_held_machine);
    RUN_TEST(test_denial_spends_its_sequence);
    RUN_TEST(test_uncommitted_intent_refuses);
    RUN_TEST(test_ambiguity_survives_retries);
    RUN_TEST(test_journal_lock_is_exclusive);
    RUN_TEST(test_dossier_rejects_hostile_input);
    RUN_TEST(test_approval_rejects_hostile_input);

    printf("\n%d/%d tests passed\n", tests_run - tests_failed, tests_run);
    return tests_failed > 0 ? 1 : 0;
}
