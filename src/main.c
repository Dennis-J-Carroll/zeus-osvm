#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zeus.h"
#include "bytecode.h"
#include "vm.h"
#include "asm.h"
#include "disasm.h"
#include "snapshot.h"
#include "policy.h"
#include "supervisor.h"
#include "jail.h"

/* Exit codes beyond 0/1 are part of the CLI contract: a caller must be able to
 * tell "parked for a human" from "refused". */
#define ZEUS_EXIT_OK      0
#define ZEUS_EXIT_ERROR   1
#define ZEUS_EXIT_ASK     3
#define ZEUS_EXIT_DENIED  4

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if ((long)fread(buf, 1, (size_t)len, f) != len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

typedef struct {
    const char *in;
    const char *out;
    const char *journal;
    const char *save;
    const char *approve;
    const char *lineage;
    long max_steps;   /* -1 = unlimited */
    bool ask;
    bool allow_raw;
    bool save_inferred; /* --save was derived, not requested */
} Options;

static void usage(const char *prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s run     <file.zeus> [options]      Run bytecode\n", prog);
    fprintf(stderr, "  %s restore <file.zsnap> [options]     Resume a suspended machine\n", prog);
    fprintf(stderr, "  %s save    <in.zeus> <out.zsnap> [--max-steps N]\n", prog);
    fprintf(stderr, "                                        Snapshot a machine (default: at entry)\n");
    fprintf(stderr, "  %s asm     <in.zasm> <out.zeus>       Assemble source to bytecode\n", prog);
    fprintf(stderr, "  %s dis     <file.zeus>                Disassemble bytecode\n", prog);
    fprintf(stderr, "  %s run-asm <in.zasm> [options]        Assemble and run\n", prog);
    fprintf(stderr, "  %s approve <dossier> [--out F]        Approve the exact machine shown in a dossier\n", prog);
    fprintf(stderr, "  %s deny    <dossier> [--journal F]   Kill a machine at its pending effect\n", prog);
    fprintf(stderr, "  %s jail    [--check] [--no-netns] [--no-seccomp] -- <cmd> [args]\n", prog);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  --max-steps N   Stop after N instructions and leave the machine suspended\n");
    fprintf(stderr, "  --journal F     Append-only JSONL audit log (intent before effect, commit after)\n");
    fprintf(stderr, "  --save F        Snapshot the machine when it stops; with --ask, the parking file\n");
    fprintf(stderr, "  --ask           Park the machine for approval before sensitive effects\n");
    fprintf(stderr, "  --approve F     Approval file to check against the pending effect (implies --ask)\n");
    fprintf(stderr, "  --allow-raw     Allow raw-socket effects (default: denied)\n");
    fprintf(stderr, "  --lineage ID    Continue an existing audit lineage\n");
    fprintf(stderr, "\nExit codes: 0 ok, 1 error, 3 parked for approval, 4 denied.\n");
}

static int parse_options(int argc, char **argv, int start, long default_steps, Options *o) {
    memset(o, 0, sizeof(*o));
    o->max_steps = default_steps;

    for (int i = start; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--max-steps") == 0) {
            if (++i >= argc) return -1;
            char *end = NULL;
            long v = strtol(argv[i], &end, 10);
            if (!end || *end != '\0' || v < 0) return -1;
            o->max_steps = v;
        } else if (strcmp(arg, "--journal") == 0) {
            if (++i >= argc) return -1;
            o->journal = argv[i];
        } else if (strcmp(arg, "--save") == 0) {
            if (++i >= argc) return -1;
            o->save = argv[i];
        } else if (strcmp(arg, "--approve") == 0) {
            if (++i >= argc) return -1;
            o->approve = argv[i];
        } else if (strcmp(arg, "--lineage") == 0) {
            if (++i >= argc) return -1;
            o->lineage = argv[i];
        } else if (strcmp(arg, "--ask") == 0) {
            o->ask = true;
        } else if (strcmp(arg, "--allow-raw") == 0) {
            o->allow_raw = true;
        } else if (!o->in) {
            o->in = arg;
        } else if (!o->out) {
            o->out = arg;
        } else {
            return -1;
        }
    }

    return o->in ? 0 : -1;
}

static void build_policy(const Options *o, bool gate, ZeusPolicy *policy) {
    zeus_policy_init(policy,
                     o->allow_raw ? ZEUS_PROFILE_FULL : ZEUS_PROFILE_STANDARD);
    /* --approve implies the gate for the same reason --ask does: the effect
     * must be the one described by the approval, and the next sensitive
     * effect must stop for its own approval instead of riding along. */
    if (gate) zeus_policy_ask_all(policy, false);
}

/* Drive a machine under a supervisor. Returns a process exit code. */
static int run_machine(ZeusVM *vm, const Options *o, const char *lineage) {
    bool gate = o->ask || o->approve != NULL;

    /* A gated run can park, so it needs a journal and a parking file. Refuse
     * up front rather than after the program has delivered effects it can no
     * longer be resumed past. */
    if (gate && (!o->journal || !o->save)) {
        fprintf(stderr, "error: the approval gate needs --journal and --save\n");
        return ZEUS_EXIT_ERROR;
    }

    ZeusPolicy policy;
    build_policy(o, gate, &policy);

    ZeusSupervisor sup;
    if (!zeus_supervisor_open(&sup, &policy, o->journal, lineage)) {
        fprintf(stderr, "error: cannot open supervisor");
        if (o->journal) fprintf(stderr, " (journal '%s')", o->journal);
        fprintf(stderr, "\n");
        return ZEUS_EXIT_ERROR;
    }

    sup.ask = gate;
    zeus_supervisor_set_paths(&sup, o->approve, o->save);

    if (sup.ambiguous) {
        fprintf(stderr,
                "error: journal ends in an uncommitted intent at seq %llu; "
                "refusing to replay an effect that may already have happened\n",
                (unsigned long long)sup.ambiguous_seq);
        zeus_supervisor_close(&sup);
        return ZEUS_EXIT_ERROR;
    }

    char why[512] = {0};
    ZeusDriveResult r = zeus_supervisor_drive(&sup, vm, o->max_steps,
                                              why, sizeof(why));

    int rc;
    switch (r) {
        case ZEUS_DRIVE_HALTED:
        case ZEUS_DRIVE_STEP_LIMIT:
            rc = ZEUS_EXIT_OK;
            break;

        case ZEUS_DRIVE_NEEDS_APPROVAL: {
            /* One file per park: an approval is refused rather than
             * overwritten, so the next park needs its own name. */
            uint64_t seq = zeus_supervisor_next_seq(&sup);
            printf("suspended: %s\n", why);
            printf("snapshot:  %s\n", sup.snapshot_path);
            printf("dossier:   %s\n", sup.dossier_path);
            printf("approve:   zeus approve %s --out %s.seq%llu.approve\n",
                   sup.dossier_path, sup.dossier_path,
                   (unsigned long long)seq);
            printf("resume:    zeus restore %s --journal %s --ask "
                   "--approve %s.seq%llu.approve\n",
                   sup.snapshot_path, o->journal, sup.dossier_path,
                   (unsigned long long)seq);
            printf("deny:      zeus deny %s --journal %s\n",
                   sup.dossier_path, o->journal);
            rc = ZEUS_EXIT_ASK;
            break;
        }

        case ZEUS_DRIVE_DENIED:
            fprintf(stderr, "denied: %s\n", why);
            rc = ZEUS_EXIT_DENIED;
            break;

        default:
            fprintf(stderr, "vm error: %s (pc=%u, sp=%lld)\n",
                    why[0] ? why : zeus_error_string(vm->error),
                    vm->pc, (long long)vm->sp);
            rc = ZEUS_EXIT_ERROR;
            break;
    }

    /* After a park the snapshot is already the parking file; rewriting it
     * would be harmless but pointless. A path that was only inferred for
     * parking is never used to overwrite the file the operator's dossier
     * points at. Every explicit --save still honours the stop. */
    if (o->save && !o->save_inferred && r != ZEUS_DRIVE_NEEDS_APPROVAL) {
        ZeusError se = zeus_vm_snapshot(vm, o->save);
        if (se != ZEUS_OK) {
            fprintf(stderr, "error saving snapshot '%s': %s\n",
                    o->save, zeus_error_string(se));
            rc = ZEUS_EXIT_ERROR;
        }
    }

    zeus_supervisor_close(&sup);
    return rc;
}

static int load_program(ZeusProgram *prog, const char *path) {
    zeus_program_init(prog);
    ZeusError err = zeus_program_load(prog, path);
    if (err != ZEUS_OK) {
        fprintf(stderr, "error loading '%s': %s\n", path, zeus_error_string(err));
        return 1;
    }
    return 0;
}

static int run_program(const ZeusProgram *prog, const Options *o,
                       const char *lineage) {
    ZeusVM vm;
    zeus_vm_init(&vm);

    ZeusError err = zeus_vm_load(&vm, prog);
    if (err != ZEUS_OK) {
        fprintf(stderr, "error loading program: %s\n", zeus_error_string(err));
        zeus_vm_free(&vm);
        return ZEUS_EXIT_ERROR;
    }

    int rc = run_machine(&vm, o, lineage);
    zeus_vm_free(&vm);
    return rc;
}

static int cmd_run(const Options *o) {
    ZeusProgram prog;
    if (load_program(&prog, o->in) != 0) return ZEUS_EXIT_ERROR;
    int rc = run_program(&prog, o, o->lineage);
    zeus_program_free(&prog);
    return rc;
}

static int cmd_restore(const Options *o) {
    /* The approval names the lineage whose accounting the pending effect
     * belongs to. Without one, the machine runs under a fresh lineage. */
    char lineage[ZEUS_ID_LEN];
    const char *lin = o->lineage;

    if (o->approve) {
        ZeusApproval a;
        if (!zeus_approval_load(o->approve, &a)) {
            fprintf(stderr, "error: cannot read approval '%s'\n", o->approve);
            return ZEUS_EXIT_ERROR;
        }
        snprintf(lineage, sizeof(lineage), "%s", a.lineage);
        lin = lineage;
    }

    /* A restore that may park needs somewhere to write the machine. The file
     * we just loaded is the natural parking file, and using it is what lets a
     * second sensitive effect park instead of failing for a missing path.
     * --approve gates even without --ask, so it needs the same fallback. */
    Options opts = *o;
    if ((o->ask || o->approve) && !opts.save) {
        opts.save = opts.in;
        opts.save_inferred = true;
    }

    ZeusVM vm;
    zeus_vm_init(&vm);

    ZeusError err = zeus_vm_restore(&vm, o->in);
    if (err != ZEUS_OK) {
        fprintf(stderr, "error restoring '%s': %s\n",
                o->in, zeus_error_string(err));
        zeus_vm_free(&vm);
        return ZEUS_EXIT_ERROR;
    }

    int rc = run_machine(&vm, &opts, lin);
    zeus_vm_free(&vm);
    return rc;
}

static int cmd_save(const Options *o) {
    if (!o->out) {
        fprintf(stderr, "error: save needs an output path\n");
        return ZEUS_EXIT_ERROR;
    }

    ZeusProgram prog;
    if (load_program(&prog, o->in) != 0) return ZEUS_EXIT_ERROR;

    Options opts = *o;
    opts.save = o->out;

    int rc = run_program(&prog, &opts, o->lineage);
    zeus_program_free(&prog);
    return rc;
}

static int cmd_approve(const char *dossier_path, const char *out_path) {
    ZeusDossier d;
    if (!zeus_dossier_load(dossier_path, &d)) {
        fprintf(stderr, "error: cannot read dossier '%s'\n", dossier_path);
        return ZEUS_EXIT_ERROR;
    }

    char default_out[ZEUS_PATH_LEN];
    if (!out_path) {
        snprintf(default_out, sizeof(default_out), "%s.approve", dossier_path);
        out_path = default_out;
    }

    if (!zeus_approval_write(&d, out_path)) {
        fprintf(stderr,
                "error: cannot write approval '%s' (does it already exist?)\n",
                out_path);
        return ZEUS_EXIT_ERROR;
    }

    printf("approved %s seq %llu\n", d.effect, (unsigned long long)d.seq);
    printf("lineage:  %s\n", d.lineage);
    printf("digest:   0x%016llx\n", (unsigned long long)d.digest);
    printf("approval: %s\n", out_path);
    return ZEUS_EXIT_OK;
}

static int cmd_deny(const Options *o) {
    /* Denial takes the dossier, not the snapshot: the dossier names the
     * lineage, sequence and digest, so the refusal is recorded in the same
     * audit chain as the approval would have been. */
    ZeusDossier d;
    if (!zeus_dossier_load(o->in, &d)) {
        fprintf(stderr, "error: cannot read dossier '%s'\n", o->in);
        return ZEUS_EXIT_ERROR;
    }
    if (d.snapshot[0] == '\0') {
        fprintf(stderr, "error: dossier does not name a snapshot\n");
        return ZEUS_EXIT_ERROR;
    }

    ZeusVM vm;
    zeus_vm_init(&vm);

    ZeusError err = zeus_vm_restore(&vm, d.snapshot);
    if (err != ZEUS_OK) {
        fprintf(stderr, "error restoring '%s': %s\n",
                d.snapshot, zeus_error_string(err));
        zeus_vm_free(&vm);
        return ZEUS_EXIT_ERROR;
    }

    if (!vm.yielded) {
        fprintf(stderr, "error: machine has no pending effect to deny\n");
        zeus_vm_free(&vm);
        return ZEUS_EXIT_ERROR;
    }

    uint64_t digest = zeus_effect_digest(&vm, &vm.pending);
    if (digest != d.digest) {
        fprintf(stderr,
                "error: dossier digest 0x%016llx does not match this machine "
                "(0x%016llx); refusing to act on a different machine\n",
                (unsigned long long)d.digest, (unsigned long long)digest);
        zeus_vm_free(&vm);
        return ZEUS_EXIT_DENIED;
    }

    ZeusPolicy policy;
    zeus_policy_init(&policy, ZEUS_PROFILE_DENY_ALL);

    ZeusSupervisor sup;
    if (!zeus_supervisor_open(&sup, &policy, o->journal, d.lineage)) {
        fprintf(stderr, "error: cannot open supervisor\n");
        zeus_vm_free(&vm);
        return ZEUS_EXIT_ERROR;
    }

    uint64_t seq = zeus_supervisor_next_seq(&sup);
    ZeusEffectKind kind = vm.pending.kind;

    if (seq != d.seq) {
        fprintf(stderr,
                "warning: journal continues at seq %llu but the dossier says "
                "%llu; the audit chain has a gap\n",
                (unsigned long long)seq, (unsigned long long)d.seq);
    }

    bool journaled = zeus_supervisor_deny(&sup, &vm, "operator_denied");

    printf("denied %s seq %llu (digest 0x%016llx); machine killed\n",
           zeus_effect_name(kind),
           (unsigned long long)seq, (unsigned long long)digest);
    if (!journaled) {
        fprintf(stderr, "warning: denial could not be journaled\n");
    }

    zeus_supervisor_close(&sup);
    zeus_vm_free(&vm);
    return ZEUS_EXIT_DENIED;
}

static int cmd_asm(const char *src_path, const char *out_path) {
    char *source = read_file(src_path);
    if (!source) {
        fprintf(stderr, "error reading '%s'\n", src_path);
        return ZEUS_EXIT_ERROR;
    }

    ZeusProgram prog;
    zeus_program_init(&prog);
    char err_buf[512];

    ZeusError err = zeus_assemble(source, &prog, err_buf, sizeof(err_buf));
    free(source);

    if (err != ZEUS_OK) {
        fprintf(stderr, "assembly error: %s\n", err_buf);
        return ZEUS_EXIT_ERROR;
    }

    err = zeus_program_save(&prog, out_path);
    if (err != ZEUS_OK) {
        fprintf(stderr, "error saving '%s': %s\n", out_path, zeus_error_string(err));
        zeus_program_free(&prog);
        return ZEUS_EXIT_ERROR;
    }

    printf("assembled '%s' -> '%s' (%u code, %u data)\n",
           src_path, out_path, prog.header.code_len, prog.header.data_len);
    zeus_program_free(&prog);
    return ZEUS_EXIT_OK;
}

static int cmd_dis(const char *path) {
    ZeusProgram prog;
    if (load_program(&prog, path) != 0) return ZEUS_EXIT_ERROR;
    zeus_disassemble(&prog, stdout);
    zeus_program_free(&prog);
    return ZEUS_EXIT_OK;
}

static int cmd_run_asm(const Options *o) {
    char *source = read_file(o->in);
    if (!source) {
        fprintf(stderr, "error reading '%s'\n", o->in);
        return ZEUS_EXIT_ERROR;
    }

    ZeusProgram prog;
    zeus_program_init(&prog);
    char err_buf[512];

    ZeusError err = zeus_assemble(source, &prog, err_buf, sizeof(err_buf));
    free(source);

    if (err != ZEUS_OK) {
        fprintf(stderr, "assembly error: %s\n", err_buf);
        return ZEUS_EXIT_ERROR;
    }

    int rc = run_program(&prog, o, o->lineage);
    zeus_program_free(&prog);
    return rc;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return ZEUS_EXIT_ERROR;
    }

    Options o;

    if (strcmp(argv[1], "run") == 0) {
        if (parse_options(argc, argv, 2, -1, &o) != 0) { usage(argv[0]); return ZEUS_EXIT_ERROR; }
        return cmd_run(&o);
    } else if (strcmp(argv[1], "restore") == 0) {
        if (parse_options(argc, argv, 2, -1, &o) != 0) { usage(argv[0]); return ZEUS_EXIT_ERROR; }
        return cmd_restore(&o);
    } else if (strcmp(argv[1], "save") == 0) {
        if (parse_options(argc, argv, 2, 0, &o) != 0) { usage(argv[0]); return ZEUS_EXIT_ERROR; }
        return cmd_save(&o);
    } else if (strcmp(argv[1], "asm") == 0) {
        if (argc < 4) { usage(argv[0]); return ZEUS_EXIT_ERROR; }
        return cmd_asm(argv[2], argv[3]);
    } else if (strcmp(argv[1], "dis") == 0) {
        if (argc < 3) { usage(argv[0]); return ZEUS_EXIT_ERROR; }
        return cmd_dis(argv[2]);
    } else if (strcmp(argv[1], "run-asm") == 0) {
        if (parse_options(argc, argv, 2, -1, &o) != 0) { usage(argv[0]); return ZEUS_EXIT_ERROR; }
        return cmd_run_asm(&o);
    } else if (strcmp(argv[1], "approve") == 0) {
        const char *dossier = NULL;
        const char *out = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--out") == 0) {
                if (++i >= argc) { usage(argv[0]); return ZEUS_EXIT_ERROR; }
                out = argv[i];
            } else if (!dossier) {
                dossier = argv[i];
            } else {
                usage(argv[0]);
                return ZEUS_EXIT_ERROR;
            }
        }
        if (!dossier) { usage(argv[0]); return ZEUS_EXIT_ERROR; }
        return cmd_approve(dossier, out);
    } else if (strcmp(argv[1], "deny") == 0) {
        if (parse_options(argc, argv, 2, -1, &o) != 0) { usage(argv[0]); return ZEUS_EXIT_ERROR; }
        return cmd_deny(&o);
    } else if (strcmp(argv[1], "jail") == 0) {
        return zeus_cmd_jail(argc - 2, argv + 2);
    } else {
        usage(argv[0]);
        return ZEUS_EXIT_ERROR;
    }
}
