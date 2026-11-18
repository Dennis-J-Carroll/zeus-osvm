#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "zeus.h"
#include "vm.h"
#include "asm.h"
#include "snapshot.h"
#include "policy.h"
#include "supervisor.h"

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
    zeus_supervisor_set_paths(&s, NULL, snap);

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
    ok = ok && zeus_approval_matches(appr, &s2, &vm, &vm.pending);

    /* A different held machine must not match the same approval. */
    ZeusVM other;
    zeus_vm_init(&other);
    ok = ok && (zeus_vm_restore(&other, snap) == ZEUS_OK);
    other.pending.value = 66;
    ok = ok && !zeus_approval_matches(appr, &s2, &other, &other.pending);
    zeus_vm_free(&other);

    /* Same lineage/seq with the wrong digest is not an approval. */
    FILE *f = fopen(bad, "w");
    ok = ok && f != NULL;
    if (f) {
        fprintf(f, "format=1\nkind=approval\nlineage=%s\nseq=1\n"
                   "digest=0x0000000000000000\n", d.lineage);
        fclose(f);
        ok = ok && !zeus_approval_matches(bad, &s2, &vm, &vm.pending);
    }

    ZeusVM resumed;
    zeus_vm_init(&resumed);
    ok = ok && (zeus_vm_restore(&resumed, snap) == ZEUS_OK);
    s2.ask = true;
    zeus_supervisor_set_paths(&s2, appr, snap);
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
    zeus_supervisor_set_paths(&s, NULL, snap);

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

int main(void) {
    printf("Running protection tests...\n");
    RUN_TEST(test_policy_defaults_to_deny);
    RUN_TEST(test_denied_effect_kills_cleanly);
    RUN_TEST(test_journal_is_append_only);
    RUN_TEST(test_sequence_survives_restore);
    RUN_TEST(test_approval_binds_the_held_machine);
    RUN_TEST(test_denial_spends_its_sequence);
    RUN_TEST(test_uncommitted_intent_refuses);

    printf("\n%d/%d tests passed\n", tests_run - tests_failed, tests_run);
    return tests_failed > 0 ? 1 : 0;
}
