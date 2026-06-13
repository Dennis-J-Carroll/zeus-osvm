#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "zeus.h"
#include "vm.h"
#include "asm.h"
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

static int test_arithmetic(void) {
    const char *src =
        "PUSH 10\n"
        "PUSH 3\n"
        "ADD\n"
        "PUSH 2\n"
        "MUL\n"
        "PUSH 7\n"
        "SUB\n"
        "HALT\n";
    ZeusProgram prog;
    ZeusVM vm;
    zeus_program_init(&prog);
    char err[256];
    if (zeus_assemble(src, &prog, err, sizeof(err)) != ZEUS_OK) {
        fprintf(stderr, "asm error: %s\n", err);
        return 0;
    }
    zeus_vm_init(&vm);
    zeus_vm_load(&vm, &prog);
    ZeusError e = zeus_vm_run(&vm);
    if (e != ZEUS_OK) {
        fprintf(stderr, "vm error: %s\n", zeus_error_string(e));
        zeus_vm_free(&vm);
        zeus_program_free(&prog);
        return 0;
    }
    /* (10+3)*2 - 7 = 19 */
    int ok = (vm.sp == 1 && zeus_vm_peek(&vm, 0) == 19);
    zeus_vm_free(&vm);
    zeus_program_free(&prog);
    return ok;
}

static int test_stack(void) {
    const char *src =
        "PUSH 1\n"
        "PUSH 2\n"
        "PUSH 3\n"
        "DUP\n"
        "SWAP\n"
        "POP\n"
        "HALT\n";
    ZeusProgram prog;
    ZeusVM vm;
    zeus_program_init(&prog);
    char err[256];
    zeus_assemble(src, &prog, err, sizeof(err));
    zeus_vm_init(&vm);
    zeus_vm_load(&vm, &prog);
    zeus_vm_run(&vm);
    /* stack: 1,2,3 -> dup 1,2,3,3 -> swap 1,2,3,3? wait top two are 3 and 3 -> same -> pop -> 1,2,3 */
    int ok = (vm.sp == 3);
    zeus_vm_free(&vm);
    zeus_program_free(&prog);
    return ok;
}

static int test_memory(void) {
    const char *src =
        "PUSH 0x1234\n"
        "PUSH 100\n"
        "STOREI\n"
        "PUSH 100\n"
        "LOADI\n"
        "HALT\n";
    ZeusProgram prog;
    ZeusVM vm;
    zeus_program_init(&prog);
    char err[256];
    zeus_assemble(src, &prog, err, sizeof(err));
    zeus_vm_init(&vm);
    zeus_vm_load(&vm, &prog);
    zeus_vm_run(&vm);
    int ok = (vm.sp == 1 && zeus_vm_peek(&vm, 0) == 0x1234);
    zeus_vm_free(&vm);
    zeus_program_free(&prog);
    return ok;
}

static int test_hello_string(void) {
    const char *src =
        ".data\n"
        "msg: .asciz \"Hi!\"\n"
        ".code\n"
        "PUSH msg\n"
        "DUP\n"
        "LOADBI\n"
        "PRINTC\n"
        "PUSH 1\n"
        "ADD\n"
        "DUP\n"
        "LOADBI\n"
        "PRINTC\n"
        "PUSH 1\n"
        "ADD\n"
        "DUP\n"
        "LOADBI\n"
        "PRINTC\n"
        "POP\n"
        "HALT\n";
    ZeusProgram prog;
    ZeusVM vm;
    zeus_program_init(&prog);
    char err[256];
    if (zeus_assemble(src, &prog, err, sizeof(err)) != ZEUS_OK) {
        fprintf(stderr, "asm error: %s\n", err);
        return 0;
    }
    zeus_vm_init(&vm);
    zeus_vm_load(&vm, &prog);
    /* Can't easily capture stdout here; just verify it runs. */
    ZeusError e = zeus_vm_run(&vm);
    int ok = (e == ZEUS_OK);
    zeus_vm_free(&vm);
    zeus_program_free(&prog);
    return ok;
}

int main(void) {
    printf("Running VM tests...\n");
    RUN_TEST(test_arithmetic);
    RUN_TEST(test_stack);
    RUN_TEST(test_memory);
    RUN_TEST(test_hello_string);

    printf("\n%d/%d tests passed\n", tests_run - tests_failed, tests_run);
    return tests_failed > 0 ? 1 : 0;
}
