#include "cpu.h"
#include "../mem/mem.h"
#include "../syscall/syscall.h"
#include <stdio.h>

// Handler function signature
typedef void (*opcode_handler)(i386 *cpu, uint8_t opcode);
static opcode_handler opcode_table[256];

// ModR/M and SIB Decoding Helpers

static void parse_modrm(i386 *cpu, ModRM *out) {
    uint8_t mrm = mem_read8(cpu->eip++);
    out->mod = (mrm >> 6) & 0x03;
    out->reg = (mrm >> 3) & 0x07;
    out->rm  = mrm & 0x07;
    out->has_sib = 0;
    out->has_disp = 0;
    out->disp = 0;

    if (out->mod != 3) {
        if (out->rm == 4) {
            out->has_sib = 1;
            cpu->eip++; // Skip SIB for now
        }
        if (out->mod == 1) {
            out->has_disp = 1;
            out->disp = (int32_t)((int8_t)mem_read8(cpu->eip++));
        } else if (out->mod == 2 || (out->mod == 0 && out->rm == 5)) {
            out->has_disp = 1;
            out->disp = mem_read32(cpu->eip);
            cpu->eip += 4;
        }
    }
}

static uint32_t resolve_rm_addr(i386 *cpu, ModRM *mrm) {
    if (mrm->mod == 3) return 0;
    uint32_t addr = (mrm->mod == 0 && mrm->rm == 5) ? mrm->disp : cpu->regs[mrm->rm];
    if (mrm->has_disp && !(mrm->mod == 0 && mrm->rm == 5)) addr += mrm->disp;
    return addr;
}

static uint32_t read_rm32(i386 *cpu, ModRM *mrm) {
    if (mrm->mod == 3) return cpu->regs[mrm->rm];
    return mem_read32(resolve_rm_addr(cpu, mrm));
}

static void write_rm32(i386 *cpu, ModRM *mrm, uint32_t val) {
    if (mrm->mod == 3) cpu->regs[mrm->rm] = val;
    else mem_write32(resolve_rm_addr(cpu, mrm), val);
}

// Opcode Handlers

static void op_unimplemented(i386 *cpu, uint8_t opcode) {
    fprintf(stderr, "Unhandled Opcode: 0x%02X at EIP: 0x%08X\n", opcode, cpu->eip - 1);
    cpu->halted = 1;
}

// 0x89: MOV r/m32, r32
static void op_mov_rm32_r32(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    write_rm32(cpu, &mrm, cpu->regs[mrm.reg]);
    cpu->cycles += 2;
}

// 0x8B: MOV r32, r/m32
static void op_mov_r32_rm32(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    cpu->regs[mrm.reg] = read_rm32(cpu, &mrm);
    cpu->cycles += 2;
}

// 0xB8 - 0xBF: MOV r32, imm32
static void op_mov_r32_imm32(i386 *cpu, uint8_t op) {
    uint8_t reg = op & 0x07;
    cpu->regs[reg] = mem_read32(cpu->eip);
    cpu->eip += 4;
    cpu->cycles += 1;
}

// 0xCD: INT imm8
static void op_int(i386 *cpu, uint8_t op) {
    (void)op;
    uint8_t vector = mem_read8(cpu->eip++);
    if (vector == 0x80) dispatch_syscall(cpu);
    else cpu->halted = 1;
    cpu->cycles += 50;
}

// Initialization and Dispatch

void init_opcode_table(void) {
    // Default to unimplemented
    for (int i = 0; i < 256; i++) {
        opcode_table[i] = op_unimplemented;
    }

    // Individual assignments
    opcode_table[0x89] = op_mov_rm32_r32;
    opcode_table[0x8B] = op_mov_r32_rm32;
    opcode_table[0xCD] = op_int;

    // Register-immediate MOV group (0xB8 through 0xBF)[cite: 1]
    for (int i = 0xB8; i <= 0xBF; i++) {
        opcode_table[i] = op_mov_r32_imm32;
    }
}

void execute_opcode(i386 *cpu, uint8_t opcode) {
    opcode_table[opcode](cpu, opcode);
}
