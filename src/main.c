#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zeus.h"
#include "bytecode.h"
#include "vm.h"
#include "asm.h"
#include "disasm.h"

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
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

static int cmd_run(const char *path) {
    ZeusProgram prog;
    zeus_program_init(&prog);

    ZeusError err = zeus_program_load(&prog, path);
    if (err != ZEUS_OK) {
        fprintf(stderr, "error loading '%s': %s\n", path, zeus_error_string(err));
        return 1;
    }

    ZeusVM vm;
    zeus_vm_init(&vm);
    err = zeus_vm_load(&vm, &prog);
    if (err != ZEUS_OK) {
        fprintf(stderr, "error loading program: %s\n", zeus_error_string(err));
        zeus_vm_free(&vm);
        zeus_program_free(&prog);
        return 1;
    }

    err = zeus_vm_run(&vm);
    if (err != ZEUS_OK) {
        fprintf(stderr, "vm error: %s (pc=%u, sp=%lld)\n",
                zeus_error_string(err), vm.pc, (long long)vm.sp);
        zeus_vm_free(&vm);
        zeus_program_free(&prog);
        return 1;
    }

    zeus_vm_free(&vm);
    zeus_program_free(&prog);
    return 0;
}

static int cmd_asm(const char *src_path, const char *out_path) {
    char *source = read_file(src_path);
    if (!source) {
        fprintf(stderr, "error reading '%s'\n", src_path);
        return 1;
    }

    ZeusProgram prog;
    zeus_program_init(&prog);
    char err_buf[512];

    ZeusError err = zeus_assemble(source, &prog, err_buf, sizeof(err_buf));
    free(source);

    if (err != ZEUS_OK) {
        fprintf(stderr, "assembly error: %s\n", err_buf);
        return 1;
    }

    err = zeus_program_save(&prog, out_path);
    if (err != ZEUS_OK) {
        fprintf(stderr, "error saving '%s': %s\n", out_path, zeus_error_string(err));
        zeus_program_free(&prog);
        return 1;
    }

    printf("assembled '%s' -> '%s' (%u code, %u data)\n",
           src_path, out_path, prog.header.code_len, prog.header.data_len);
    zeus_program_free(&prog);
    return 0;
}

static int cmd_dis(const char *path) {
    ZeusProgram prog;
    zeus_program_init(&prog);

    ZeusError err = zeus_program_load(&prog, path);
    if (err != ZEUS_OK) {
        fprintf(stderr, "error loading '%s': %s\n", path, zeus_error_string(err));
        return 1;
    }

    zeus_disassemble(&prog, stdout);
    zeus_program_free(&prog);
    return 0;
}

static int cmd_run_asm(const char *src_path) {
    char *source = read_file(src_path);
    if (!source) {
        fprintf(stderr, "error reading '%s'\n", src_path);
        return 1;
    }

    ZeusProgram prog;
    zeus_program_init(&prog);
    char err_buf[512];

    ZeusError err = zeus_assemble(source, &prog, err_buf, sizeof(err_buf));
    free(source);

    if (err != ZEUS_OK) {
        fprintf(stderr, "assembly error: %s\n", err_buf);
        return 1;
    }

    ZeusVM vm;
    zeus_vm_init(&vm);
    err = zeus_vm_load(&vm, &prog);
    if (err != ZEUS_OK) {
        fprintf(stderr, "error loading program: %s\n", zeus_error_string(err));
        zeus_vm_free(&vm);
        zeus_program_free(&prog);
        return 1;
    }

    err = zeus_vm_run(&vm);
    if (err != ZEUS_OK) {
        fprintf(stderr, "vm error: %s (pc=%u, sp=%lld)\n",
                zeus_error_string(err), vm.pc, (long long)vm.sp);
        zeus_vm_free(&vm);
        zeus_program_free(&prog);
        return 1;
    }

    zeus_vm_free(&vm);
    zeus_program_free(&prog);
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s run <file.zeus>        Run a bytecode file\n", prog);
    fprintf(stderr, "  %s asm <file.zasm> <out.zeus>  Assemble source to bytecode\n", prog);
    fprintf(stderr, "  %s dis <file.zeus>        Disassemble bytecode\n", prog);
    fprintf(stderr, "  %s run-asm <file.zasm>    Assemble and run source\n", prog);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "run") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        return cmd_run(argv[2]);
    } else if (strcmp(argv[1], "asm") == 0) {
        if (argc < 4) { usage(argv[0]); return 1; }
        return cmd_asm(argv[2], argv[3]);
    } else if (strcmp(argv[1], "dis") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        return cmd_dis(argv[2]);
    } else if (strcmp(argv[1], "run-asm") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        return cmd_run_asm(argv[2]);
    } else {
        usage(argv[0]);
        return 1;
    }
}
