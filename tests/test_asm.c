#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "zeus.h"
#include "asm.h"
#include "disasm.h"
#include "bytecode.h"

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

static int test_label_resolution(void) {
    const char *src =
        "PUSH 0\n"
        "loop:\n"
        "PUSH 1\n"
        "ADD\n"
        "DUP\n"
        "PUSH 5\n"
        "LT\n"
        "JNZ loop\n"
        "HALT\n";
    ZeusProgram prog;
    zeus_program_init(&prog);
    char err[256];
    ZeusError e = zeus_assemble(src, &prog, err, sizeof(err));
    if (e != ZEUS_OK) {
        fprintf(stderr, "asm error: %s\n", err);
        return 0;
    }
    int ok = (prog.header.code_len > 0);
    zeus_program_free(&prog);
    return ok;
}

static int test_directives(void) {
    const char *src =
        ".data\n"
        "x: .byte 0x41\n"
        "y: .word 0x1234\n"
        "z: .asciz \"hi\"\n"
        ".code\n"
        "LOADB x\n"
        "HALT\n";
    ZeusProgram prog;
    zeus_program_init(&prog);
    char err[256];
    ZeusError e = zeus_assemble(src, &prog, err, sizeof(err));
    if (e != ZEUS_OK) {
        fprintf(stderr, "asm error: %s\n", err);
        return 0;
    }
    int ok = (prog.header.data_len >= 11); /* 1 + 8 + 3 */
    zeus_program_free(&prog);
    return ok;
}

static int test_disasm_roundtrip(void) {
    const char *src =
        "PUSH 42\n"
        "PUSH 0x1234\n"
        "ADD\n"
        "HALT\n";
    ZeusProgram prog;
    zeus_program_init(&prog);
    char err[256];
    if (zeus_assemble(src, &prog, err, sizeof(err)) != ZEUS_OK) {
        fprintf(stderr, "asm error: %s\n", err);
        return 0;
    }
    FILE *fp = tmpfile();
    if (!fp) return 0;
    ZeusError e = zeus_disassemble(&prog, fp);
    if (e != ZEUS_OK) {
        fclose(fp);
        zeus_program_free(&prog);
        return 0;
    }
    rewind(fp);
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    int ok = (strstr(buf, "PUSH") != NULL && strstr(buf, "ADD") != NULL);
    zeus_program_free(&prog);
    return ok;
}

int main(void) {
    printf("Running assembler tests...\n");
    RUN_TEST(test_label_resolution);
    RUN_TEST(test_directives);
    RUN_TEST(test_disasm_roundtrip);

    printf("\n%d/%d tests passed\n", tests_run - tests_failed, tests_run);
    return tests_failed > 0 ? 1 : 0;
}
