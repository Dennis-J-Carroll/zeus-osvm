#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
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

static int run_source(const char *src, ZeusVM *vm, ZeusProgram *prog) {
    char err[256];
    zeus_program_init(prog);
    if (zeus_assemble(src, prog, err, sizeof(err)) != ZEUS_OK) {
        fprintf(stderr, "asm error: %s\n", err);
        return 0;
    }
    zeus_vm_init(vm);
    if (zeus_vm_load(vm, prog) != ZEUS_OK) {
        return 0;
    }
    zeus_vm_run(vm);
    return 1;
}

static int test_add_wraps(void) {
    const char *src = "PUSH 9223372036854775807\nPUSH 1\nADD\nHALT\n";
    ZeusProgram prog;
    ZeusVM vm;
    if (!run_source(src, &vm, &prog)) return 0;

    int ok = (vm.error == ZEUS_OK && vm.sp == 1 &&
              zeus_vm_peek(&vm, 0) == INT64_MIN);

    zeus_vm_free(&vm);
    zeus_program_free(&prog);
    return ok;
}

static int test_div_overflow_wraps(void) {
    const char *src = "PUSH -9223372036854775808\nPUSH -1\nDIV\nHALT\n";
    ZeusProgram prog;
    ZeusVM vm;
    if (!run_source(src, &vm, &prog)) return 0;

    int ok = (vm.error == ZEUS_OK && vm.sp == 1 &&
              zeus_vm_peek(&vm, 0) == INT64_MIN);

    zeus_vm_free(&vm);
    zeus_program_free(&prog);
    return ok;
}

static int test_shifts_are_masked(void) {
    const char *src =
        "PUSH 1\n"
        "PUSH 64\n"          /* masked to 0 */
        "SHL\n"
        "PUSH -8\n"
        "PUSH 1\n"
        "SHR\n"
        "HALT\n";
    ZeusProgram prog;
    ZeusVM vm;
    if (!run_source(src, &vm, &prog)) return 0;

    int ok = (vm.error == ZEUS_OK && vm.sp == 2 &&
              zeus_vm_peek(&vm, 1) == 1 &&
              zeus_vm_peek(&vm, 0) == -4);

    zeus_vm_free(&vm);
    zeus_program_free(&prog);
    return ok;
}

static int test_neg_wraps(void) {
    const char *src = "PUSH -9223372036854775808\nNEG\nHALT\n";
    ZeusProgram prog;
    ZeusVM vm;
    if (!run_source(src, &vm, &prog)) return 0;

    int ok = (vm.error == ZEUS_OK && vm.sp == 1 &&
              zeus_vm_peek(&vm, 0) == INT64_MIN);

    zeus_vm_free(&vm);
    zeus_program_free(&prog);
    return ok;
}

static int test_recv_huge_len_rejected(void) {
    /* 4294967296 used to truncate to 0 and let RECV write 64 KiB over a
     * 1-byte reservation. It must be a bounds error before any I/O. */
    const char *src =
        "PUSH 0\n"
        "PUSH 0\n"
        "PUSH 4294967296\n"
        "RECV\n"
        "HALT\n";
    ZeusProgram prog;
    ZeusVM vm;
    if (!run_source(src, &vm, &prog)) return 0;

    int ok = (vm.error == ZEUS_ERR_OUT_OF_BOUNDS);

    zeus_vm_free(&vm);
    zeus_program_free(&prog);
    return ok;
}

static int test_recv_negative_len_rejected(void) {
    const char *src =
        "PUSH 0\n"
        "PUSH 0\n"
        "PUSH -1\n"
        "RECV\n"
        "HALT\n";
    ZeusProgram prog;
    ZeusVM vm;
    if (!run_source(src, &vm, &prog)) return 0;

    int ok = (vm.error == ZEUS_ERR_OUT_OF_BOUNDS);

    zeus_vm_free(&vm);
    zeus_program_free(&prog);
    return ok;
}

static int test_send_past_memory_rejected(void) {
    const char *src =
        "PUSH 0\n"
        "PUSH 65536\n"
        "PUSH 8\n"
        "SEND\n"
        "HALT\n";
    ZeusProgram prog;
    ZeusVM vm;
    if (!run_source(src, &vm, &prog)) return 0;

    int ok = (vm.error == ZEUS_ERR_OUT_OF_BOUNDS);

    zeus_vm_free(&vm);
    zeus_program_free(&prog);
    return ok;
}

static int test_loadi_huge_address_rejected(void) {
    /* 2^32 used to cast to 0 and pass the bounds check. */
    const char *src = "PUSH 4294967296\nLOADI\nHALT\n";
    ZeusProgram prog;
    ZeusVM vm;
    if (!run_source(src, &vm, &prog)) return 0;

    int ok = (vm.error == ZEUS_ERR_OUT_OF_BOUNDS);

    zeus_vm_free(&vm);
    zeus_program_free(&prog);
    return ok;
}

static int test_unterminated_hostname_rejected(void) {
    const char *src =
        "PUSH 0\n"     /* fd */
        "PUSH 0\n"     /* host string at address 0 */
        "PUSH 80\n"    /* port */
        "CONNECT\n"
        "HALT\n";

    ZeusProgram prog;
    ZeusVM vm;
    char err[256];

    zeus_program_init(&prog);
    if (zeus_assemble(src, &prog, err, sizeof(err)) != ZEUS_OK) return 0;

    zeus_vm_init(&vm);
    if (zeus_vm_load(&vm, &prog) != ZEUS_OK) return 0;

    /* No NUL anywhere: the old code handed this straight to gethostbyname. */
    memset(vm.memory, 'A', sizeof(vm.memory));
    zeus_vm_run(&vm);

    int ok = (vm.error == ZEUS_ERR_OUT_OF_BOUNDS);

    zeus_vm_free(&vm);
    zeus_program_free(&prog);
    return ok;
}

static int test_header_is_17_bytes(void) {
    const char *src = "PUSH 1\nHALT\n";
    const char *path = "/tmp/zeus_header_test.zeus";

    ZeusProgram prog;
    char err[256];
    zeus_program_init(&prog);
    if (zeus_assemble(src, &prog, err, sizeof(err)) != ZEUS_OK) return 0;
    if (zeus_program_save(&prog, path) != ZEUS_OK) {
        zeus_program_free(&prog);
        return 0;
    }

    FILE *f = fopen(path, "rb");
    if (!f) { zeus_program_free(&prog); return 0; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);

    long expected = (long)ZEUS_HEADER_SIZE + (long)prog.header.code_len
                  + (long)prog.header.data_len;

    ZeusProgram back;
    zeus_program_init(&back);
    ZeusError e = zeus_program_load(&back, path);

    int ok = (size == expected) &&
             (e == ZEUS_OK) &&
             (back.header.code_len == prog.header.code_len) &&
             (back.header.version == ZEUS_VERSION);

    zeus_program_free(&back);
    zeus_program_free(&prog);
    remove(path);
    return ok;
}

static int test_rejects_padded_header(void) {
    /* The old on-disk layout was a padded 20-byte struct. Such a file must
     * be rejected, not misread. */
    const char *path = "/tmp/zeus_padded_header.zeus";
    uint8_t buf[22];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, "ZEUS", 4);
    buf[4] = ZEUS_VERSION;
    /* buf[5..7] are the old alignment padding */
    buf[12] = 2;    /* code_len low byte, at the old struct offset */
    buf[20] = 0x00; /* HALT */
    buf[21] = 0x00;

    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(buf, 1, sizeof(buf), f);
    fclose(f);

    ZeusProgram prog;
    zeus_program_init(&prog);
    ZeusError e = zeus_program_load(&prog, path);
    zeus_program_free(&prog);
    remove(path);

    return e == ZEUS_ERR_BAD_BYTECODE;
}

static int test_rejects_oversized_data(void) {
    const char *src =
        ".data\n"
        ".org 70000\n"
        ".byte 1\n"
        ".code\n"
        "HALT\n";

    ZeusProgram prog;
    char err[256];
    zeus_program_init(&prog);
    ZeusError e = zeus_assemble(src, &prog, err, sizeof(err));
    zeus_program_free(&prog);

    return e == ZEUS_ERR_BAD_BYTECODE;
}

int main(void) {
    printf("Running safety tests...\n");
    RUN_TEST(test_add_wraps);
    RUN_TEST(test_div_overflow_wraps);
    RUN_TEST(test_shifts_are_masked);
    RUN_TEST(test_neg_wraps);
    RUN_TEST(test_recv_huge_len_rejected);
    RUN_TEST(test_recv_negative_len_rejected);
    RUN_TEST(test_send_past_memory_rejected);
    RUN_TEST(test_loadi_huge_address_rejected);
    RUN_TEST(test_unterminated_hostname_rejected);
    RUN_TEST(test_header_is_17_bytes);
    RUN_TEST(test_rejects_padded_header);
    RUN_TEST(test_rejects_oversized_data);

    printf("\n%d/%d tests passed\n", tests_run - tests_failed, tests_run);
    return tests_failed > 0 ? 1 : 0;
}
