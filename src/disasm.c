#include "disasm.h"
#include "isa.h"
#include <string.h>

static int64_t read_i64(const uint8_t *p) {
    uint64_t v = (uint64_t)p[0]
               | ((uint64_t)p[1] << 8)
               | ((uint64_t)p[2] << 16)
               | ((uint64_t)p[3] << 24)
               | ((uint64_t)p[4] << 32)
               | ((uint64_t)p[5] << 40)
               | ((uint64_t)p[6] << 48)
               | ((uint64_t)p[7] << 56);
    return (int64_t)v;
}

ZeusError zeus_disassemble(const ZeusProgram *prog, FILE *out) {
    if (!prog || !out) return ZEUS_ERR_IO;

    fprintf(out, "; Zeus_osVM disassembly\n");
    fprintf(out, "; version: %u, entry: %u, code: %u, data: %u\n",
            prog->header.version,
            prog->header.entry_point,
            prog->header.code_len,
            prog->header.data_len);

    uint32_t pc = 0;
    while (pc < prog->header.code_len) {
        uint8_t op = prog->code[pc];
        const ZeusOpInfo *info = zeus_op_info((ZeusOpcode)op);

        if (!info) {
            fprintf(out, "%04x: DB 0x%02x\n", pc, op);
            pc++;
            continue;
        }

        if (info->has_operand) {
            if (pc + 9 > prog->header.code_len) {
                fprintf(out, "%04x: ; truncated instruction\n", pc);
                break;
            }
            int64_t operand = read_i64(prog->code + pc + 1);
            fprintf(out, "%04x: %-12s %lld\n", pc, info->name, (long long)operand);
            pc += 9;
        } else {
            fprintf(out, "%04x: %s\n", pc, info->name);
            pc++;
        }
    }

    if (prog->header.data_len > 0) {
        fprintf(out, "\n; data section (%u bytes)\n", prog->header.data_len);
        uint32_t i = 0;
        while (i < prog->header.data_len) {
            fprintf(out, "%04x: ", i);
            for (uint32_t j = 0; j < 16 && i + j < prog->header.data_len; j++) {
                fprintf(out, "%02x ", prog->data[i + j]);
            }
            fprintf(out, " ");
            for (uint32_t j = 0; j < 16 && i + j < prog->header.data_len; j++) {
                uint8_t c = prog->data[i + j];
                fputc((c >= 32 && c < 127) ? c : '.', out);
            }
            fprintf(out, "\n");
            i += 16;
        }
    }

    return ZEUS_OK;
}
