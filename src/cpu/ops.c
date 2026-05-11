#include "cpu.h"
#include "../mem/mem.h"
#include "../syscall/syscall.h"
#include <stdio.h>

#define FLAG_CF (1 << 0)
#define FLAG_ZF (1 << 6)
#define FLAG_SF (1 << 7)
#define FLAG_OF (1 << 11)

static int g_operand_size_16 = 0;
static uint32_t g_gs_base = 0;

void set_gs_base(uint32_t base) { g_gs_base = base; }
uint32_t get_gs_base(void) { return g_gs_base; }

typedef void (*opcode_handler)(i386 *cpu, uint8_t opcode);
static opcode_handler opcode_table[256];

typedef void (*opcode_handler_0f)(i386 *cpu, uint8_t opcode);
static opcode_handler_0f opcode_table_0f[256];

static void set_cf(i386 *cpu, int cf) {
    if (cf) cpu->eflags |= FLAG_CF;
    else cpu->eflags &= ~FLAG_CF;
}

static void set_zf(i386 *cpu, uint32_t result) {
    if (result == 0) cpu->eflags |= FLAG_ZF;
    else cpu->eflags &= ~FLAG_ZF;
}

static void set_sf(i386 *cpu, uint32_t result) {
    if (result & 0x80000000) cpu->eflags |= FLAG_SF;
    else cpu->eflags &= ~FLAG_SF;
}

static void set_of_add(i386 *cpu, uint32_t a, uint32_t b, uint32_t result) {
    // overflow if two positives give negative or two negatives give positive
    int of = (~(a ^ b) & (a ^ result)) >> 31;
    if (of) cpu->eflags |= FLAG_OF;
    else cpu->eflags &= ~FLAG_OF;
}

static void set_of_sub(i386 *cpu, uint32_t a, uint32_t b, uint32_t result) {
    // overflow if signs of operands differ and result sign differs from a
    int of = ((a ^ b) & (a ^ result)) >> 31;
    if (of) cpu->eflags |= FLAG_OF;
    else cpu->eflags &= ~FLAG_OF;
}

static int get_cf(i386 *cpu) { return (cpu->eflags >> 0)  & 1; }
static int get_zf(i386 *cpu) { return (cpu->eflags >> 6)  & 1; }
static int get_sf(i386 *cpu) { return (cpu->eflags >> 7)  & 1; }
static int get_of(i386 *cpu) { return (cpu->eflags >> 11) & 1; }

// modrm and addressing helpers

static void parse_modrm(i386 *cpu, ModRM *out) {
    uint8_t mrm = mem_read8(cpu->eip++);
    out->mod = (mrm >> 6) & 0x03;
    out->reg = (mrm >> 3) & 0x07;
    out->rm = mrm & 0x07;
    out->has_sib = 0;
    out->has_disp = 0;
    out->disp = 0;

    if (out->mod != 3) {
        if (out->rm == 4) {
            out->has_sib = 1;
            uint8_t sib = mem_read8(cpu->eip++);
            uint8_t scale = (sib >> 6) & 0x03;
            uint8_t index = (sib >> 3) & 0x07;
            uint8_t base  = sib & 0x07;
            // for now handle the common cases: [base + disp] with no index (index=4 means no index)
            out->sib_base  = base;
            out->sib_index = index;
            out->sib_scale = scale;
        }
        if (out->mod == 1) {
            // 8 bit displacement, sign extend it
            out->has_disp = 1;
            out->disp = (int32_t)((int8_t)mem_read8(cpu->eip++));
        } else if (out->mod == 2 || (out->mod == 0 && out->rm == 5)) {
            // 32 bit displacement or disp32 with no base (mod=0 rm=5)
            out->has_disp = 1;
            out->disp = mem_read32(cpu->eip);
            cpu->eip += 4;
        }
    }
}

static uint32_t resolve_rm_addr(i386 *cpu, ModRM *mrm) {
    if (mrm->mod == 3) return 0;

    uint32_t addr = 0;
    if (mrm->has_sib) {
        // base=5 with mod=0 means disp32 with no base register
        if (mrm->sib_base == 5 && mrm->mod == 0) {
            addr = 0;
        } else {
            addr = cpu->regs[mrm->sib_base];
        }
        if (mrm->sib_index != 4) {
            addr += cpu->regs[mrm->sib_index] << mrm->sib_scale;
        }
    } else {
        addr = (mrm->mod == 0 && mrm->rm == 5) ? 0 : cpu->regs[mrm->rm];
    }

    if (mrm->has_disp) addr += mrm->disp;
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

// opcode handlers

static void op_unimplemented(i386 *cpu, uint8_t opcode) {
    fprintf(stderr, "unhandled opcode 0x%02X at EIP 0x%08X\n", opcode, cpu->eip - 1);
    cpu->halted = 1;
}

static void op_0f_unimplemented(i386 *cpu, uint8_t opcode) {
    fprintf(stderr, "unhandled 0x0F opcode 0x%02X at EIP 0x%08X\n", opcode, cpu->eip - 2);
    cpu->halted = 1;
}

static void op_prefix_0f(i386 *cpu, uint8_t op) {
    (void)op;
    uint8_t op2 = mem_read8(cpu->eip++);
    opcode_table_0f[op2](cpu, op2);
}

// 0x0F prefix

// 0x0F 0x4x CMOVcc r32, r/m32
static void op_cmovcc(i386 *cpu, uint8_t op) {
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint32_t src = read_rm32(cpu, &mrm);
    int taken = 0;
    switch (op) {
        case 0x40: taken =  get_of(cpu); break; // CMOVO
        case 0x41: taken = !get_of(cpu); break; // CMOVNO
        case 0x42: taken =  get_cf(cpu); break; // CMOVB
        case 0x43: taken = !get_cf(cpu); break; // CMOVNB
        case 0x44: taken =  get_zf(cpu); break; // CMOVE
        case 0x45: taken = !get_zf(cpu); break; // CMOVNE
        case 0x46: taken =  get_cf(cpu) || get_zf(cpu);  break; // CMOVBE
        case 0x47: taken = !get_cf(cpu) && !get_zf(cpu); break; // CMOVA
        case 0x48: taken =  get_sf(cpu); break; // CMOVS
        case 0x49: taken = !get_sf(cpu); break; // CMOVNS
        case 0x4A: taken = 0; break; // CMOVP stub, parity flag not implemented
        case 0x4B: taken = 1; break; // CMOVNP stub
        case 0x4C: taken =  get_sf(cpu) != get_of(cpu); break; // CMOVL
        case 0x4D: taken =  get_sf(cpu) == get_of(cpu); break; // CMOVGE
        case 0x4E: taken =  get_zf(cpu) || (get_sf(cpu) != get_of(cpu)); break; // CMOVLE
        case 0x4F: taken = !get_zf(cpu) && (get_sf(cpu) == get_of(cpu)); break; // CMOVG
        default:
            fprintf(stderr, "unhandled CMOVcc op=0x%02X\n", op);
            cpu->halted = 1;
            return;
    }
    if (taken) cpu->regs[mrm.reg] = src;
    cpu->cycles += 2;
}

// 0x0F 0xAF IMUL r32, r/m32
static void op_imul_r32_rm32(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    int32_t a = (int32_t)cpu->regs[mrm.reg];
    int32_t b = (int32_t)read_rm32(cpu, &mrm);
    int32_t result = a * b;
    cpu->regs[mrm.reg] = (uint32_t)result;
    set_zf(cpu, (uint32_t)result);
    set_sf(cpu, (uint32_t)result);
    cpu->cycles += 10;
}

// 0x0F 0x8x JCC near
static void op_jcc_near(i386 *cpu, uint8_t op) {
    int32_t offset = (int32_t)mem_read32(cpu->eip);
    cpu->eip += 4;
    int taken = 0;
    switch (op) {
        case 0x80: taken =  get_of(cpu); break; // JO
        case 0x81: taken = !get_of(cpu); break; // JNO
        case 0x82: taken =  get_cf(cpu); break; // JB
        case 0x83: taken = !get_cf(cpu); break; // JNB
        case 0x84: taken =  get_zf(cpu); break; // JE
        case 0x85: taken = !get_zf(cpu); break; // JNE
        case 0x86: taken =  get_cf(cpu) || get_zf(cpu);  break; // JBE
        case 0x87: taken = !get_cf(cpu) && !get_zf(cpu); break; // JA
        case 0x88: taken =  get_sf(cpu); break; // JS
        case 0x89: taken = !get_sf(cpu); break; // JNS
        case 0x8C: taken =  get_sf(cpu) != get_of(cpu); break; // JL
        case 0x8D: taken =  get_sf(cpu) == get_of(cpu); break; // JGE
        case 0x8E: taken =  get_zf(cpu) || (get_sf(cpu) != get_of(cpu)); break; // JLE
        case 0x8F: taken = !get_zf(cpu) && (get_sf(cpu) == get_of(cpu)); break; // JG
        default:
            fprintf(stderr, "unhandled Jcc near op=0x%02X\n", op);
            cpu->halted = 1;
            return;
    }
    if (taken) cpu->eip += offset;
    cpu->cycles += 1;
}

// 0x0F 0xB6 MOVZX r32, r/m8
static void op_movzx_r32_rm8(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint8_t val = (mrm.mod == 3)
        ? (uint8_t)cpu->regs[mrm.rm]
        : mem_read8(resolve_rm_addr(cpu, &mrm));
    cpu->regs[mrm.reg] = (uint32_t)val;
    cpu->cycles += 2;
}

// 0x00-0xFF opcodes

// 0x01 ADD r/m32, r32
static void op_add_rm32_r32(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint32_t a = read_rm32(cpu, &mrm);
    uint32_t b = cpu->regs[mrm.reg];
    uint32_t result = a + b;
    write_rm32(cpu, &mrm, result);
    set_cf(cpu, result < a);
    set_zf(cpu, result);
    set_sf(cpu, result);
    set_of_add(cpu, a, b, result);
    cpu->cycles += 2;
}

// 0x03 ADD r32, r/m32
static void op_add_r32_rm32(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint32_t a = cpu->regs[mrm.reg];
    uint32_t b = read_rm32(cpu, &mrm);
    uint32_t result = a + b;
    cpu->regs[mrm.reg] = result;
    set_cf(cpu, result < a);
    set_zf(cpu, result);
    set_sf(cpu, result);
    set_of_add(cpu, a, b, result);
    cpu->cycles += 2;
}

// 0x05 ADD eax, imm32
static void op_add_eax_imm32(i386 *cpu, uint8_t op) {
    (void)op;
    uint32_t a = cpu->regs[REG_EAX];
    uint32_t b = mem_read32(cpu->eip);
    cpu->eip += 4;
    uint32_t result = a + b;
    cpu->regs[REG_EAX] = result;
    set_cf(cpu, result < a);
    set_zf(cpu, result);
    set_sf(cpu, result);
    set_of_add(cpu, a, b, result);
    cpu->cycles += 1;
}

// 0x29 SUB r/m32, r32
static void op_sub_rm32_r32(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint32_t a = read_rm32(cpu, &mrm);
    uint32_t b = cpu->regs[mrm.reg];
    uint32_t result = a - b;
    write_rm32(cpu, &mrm, result);
    set_cf(cpu, a < b);
    set_zf(cpu, result);
    set_sf(cpu, result);
    set_of_sub(cpu, a, b, result);
    cpu->cycles += 2;
}

// 0x2B SUB r32, r/m32
static void op_sub_r32_rm32(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint32_t a = cpu->regs[mrm.reg];
    uint32_t b = read_rm32(cpu, &mrm);
    uint32_t result = a - b;
    cpu->regs[mrm.reg] = result;
    set_cf(cpu, a < b);
    set_zf(cpu, result);
    set_sf(cpu, result);
    set_of_sub(cpu, a, b, result);
    cpu->cycles += 2;
}

// 0x31 XOR r/m32, r32
static void op_xor_rm32_r32(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint32_t result = read_rm32(cpu, &mrm) ^ cpu->regs[mrm.reg];
    write_rm32(cpu, &mrm, result);
    set_zf(cpu, result);
    set_sf(cpu, result);
    cpu->cycles += 2;
}

// 0x39 CMP r/m32, r32
static void op_cmp_rm32_r32(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint32_t a = read_rm32(cpu, &mrm);
    uint32_t b = cpu->regs[mrm.reg];
    uint32_t result = a - b;
    set_cf(cpu, a < b);
    set_zf(cpu, result);
    set_sf(cpu, result);
    set_of_sub(cpu, a, b, result);
    cpu->cycles += 2;
}

// 0x3B CMP r32, r/m32
static void op_cmp_r32_rm32(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint32_t a = cpu->regs[mrm.reg];
    uint32_t b = read_rm32(cpu, &mrm);
    uint32_t result = a - b;
    set_cf(cpu, a < b);
    set_zf(cpu, result);
    set_sf(cpu, result);
    set_of_sub(cpu, a, b, result);
    cpu->cycles += 2;
}

// 0x40-0x47 INC r32
static void op_inc_r32(i386 *cpu, uint8_t op) {
    uint8_t reg = op & 0x07;
    cpu->regs[reg]++;
    set_zf(cpu, cpu->regs[reg]);
    set_sf(cpu, cpu->regs[reg]);
    cpu->cycles += 1;
}

// 0x48-0x4F DEC r32
static void op_dec_r32(i386 *cpu, uint8_t op) {
    uint8_t reg = op & 0x07;
    cpu->regs[reg]--;
    set_zf(cpu, cpu->regs[reg]);
    set_sf(cpu, cpu->regs[reg]);
    cpu->cycles += 1;
}

// 0x50-0x57 PUSH r32
static void op_push_r32(i386 *cpu, uint8_t op) {
    uint8_t reg = op & 0x07;
    cpu->regs[REG_ESP] -= 4;
    mem_write32(cpu->regs[REG_ESP], cpu->regs[reg]);
    cpu->cycles += 1;
}

// 0x58-0x5F POP r32
static void op_pop_r32(i386 *cpu, uint8_t op) {
    uint8_t reg = op & 0x07;
    cpu->regs[reg] = mem_read32(cpu->regs[REG_ESP]);
    cpu->regs[REG_ESP] += 4;
    cpu->cycles += 1;
}

// 0x65 GS segment override prefix
static void op_prefix_gs(i386 *cpu, uint8_t op) {
    (void)op;
    uint8_t next = mem_read8(cpu->eip++);
    // bias the next memory access by gs_base
    // we do this by temporarily storing it and letting resolve_rm_addr add it
    // for now handle the most common case: MOV r32, GS:[addr]
    if (next == 0x8B) {
        ModRM mrm;
        parse_modrm(cpu, &mrm);
        uint32_t addr = resolve_rm_addr(cpu, &mrm) + g_gs_base;
        cpu->regs[mrm.reg] = mem_read32(addr);
    } else if (next == 0x89) {
        ModRM mrm;
        parse_modrm(cpu, &mrm);
        uint32_t addr = resolve_rm_addr(cpu, &mrm) + g_gs_base;
        mem_write32(addr, cpu->regs[mrm.reg]);
    } else {
        // fallback, execute without segment override
        fprintf(stderr, "GS prefix with unhandled opcode 0x%02X, ignoring segment\n", next);
        execute_opcode(cpu, next);
    }
    cpu->cycles += 1;
}

// 0x66 operand size prefix
static void op_prefix_66(i386 *cpu, uint8_t op) {
    (void)op;
    g_operand_size_16 = 1;
    // fetch and execute the next instruction with the prefix active
    uint8_t next = mem_read8(cpu->eip++);
    execute_opcode(cpu, next);
    g_operand_size_16 = 0;
}

// 0x68 PUSH imm32
static void op_push_imm32(i386 *cpu, uint8_t op) {
    (void)op;
    uint32_t imm = mem_read32(cpu->eip);
    cpu->eip += 4;
    cpu->regs[REG_ESP] -= 4;
    mem_write32(cpu->regs[REG_ESP], imm);
    cpu->cycles += 1;
}

// 0x6A PUSH imm8
static void op_push_imm8(i386 *cpu, uint8_t op) {
    (void)op;
    int32_t imm = (int32_t)(int8_t)mem_read8(cpu->eip++);
    cpu->regs[REG_ESP] -= 4;
    mem_write32(cpu->regs[REG_ESP], (uint32_t)imm);
    cpu->cycles += 1;
}

// 0x74-0x7D JCC short
static void op_jcc_short(i386 *cpu, uint8_t op) {
    int8_t offset = (int8_t)mem_read8(cpu->eip++);
    int taken = 0;
    switch (op) {
        case 0x70: taken =  get_of(cpu); break; // JO
        case 0x71: taken = !get_of(cpu); break; // JNO
        case 0x72: taken =  get_cf(cpu); break; // JB/JNAE
        case 0x73: taken = !get_cf(cpu); break; // JNB/JAE
        case 0x74: taken =  get_zf(cpu); break; // JE/JZ
        case 0x75: taken = !get_zf(cpu); break; // JNE/JNZ
        case 0x76: taken =  get_cf(cpu) || get_zf(cpu);  break; // JBE
        case 0x77: taken = !get_cf(cpu) && !get_zf(cpu); break; // JA
        case 0x78: taken =  get_sf(cpu); break; // JS
        case 0x79: taken = !get_sf(cpu); break; // JNS
        case 0x7C: taken =  get_sf(cpu) != get_of(cpu); break; // JL
        case 0x7D: taken =  get_sf(cpu) == get_of(cpu); break; // JGE
        case 0x7E: taken =  get_zf(cpu) || (get_sf(cpu) != get_of(cpu)); break; // JLE
        case 0x7F: taken = !get_zf(cpu) && (get_sf(cpu) == get_of(cpu)); break; // JG
        default:
            fprintf(stderr, "unhandled Jcc short op=0x%02X\n", op);
            cpu->halted = 1;
            return;
    }
    if (taken) cpu->eip += offset;
    cpu->cycles += 1;
}

// 0x81 ADD/SUB/CMP r/m32, imm32
static void op_81(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint32_t imm = mem_read32(cpu->eip);
    cpu->eip += 4;
    uint32_t a = read_rm32(cpu, &mrm);
    uint32_t result = 0;
    switch (mrm.reg) {
        case 0:
            result = a + imm;
            write_rm32(cpu, &mrm, result);
            set_cf(cpu, result < a);
            set_of_add(cpu, a, imm, result);
            break;
        case 1:
            result = a | imm;
            write_rm32(cpu, &mrm, result);
            set_cf(cpu, 0);
            break;
        case 4:
            result = a & imm;
            write_rm32(cpu, &mrm, result);
            set_cf(cpu, 0);
            break;
        case 5:
            result = a - imm;
            write_rm32(cpu, &mrm, result);
            set_cf(cpu, a < imm);
            set_of_sub(cpu, a, imm, result);
            break;
        case 6:
            result = a ^ imm;
            write_rm32(cpu, &mrm, result);
            set_cf(cpu, 0);
            break;
        case 7: // CMP
            result = a - imm;
            set_cf(cpu, a < imm);
            set_of_sub(cpu, a, imm, result);
            break;
        default:
            fprintf(stderr, "unhandled 0x81 reg=%d\n", mrm.reg);
            cpu->halted = 1;
            return;
    }
    set_zf(cpu, result);
    set_sf(cpu, result);
    cpu->cycles += 2;
}

// 0x83 ADD/SUB/CMP r/m32, imm8 (sign extended)
static void op_83(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint32_t imm = (uint32_t)(int32_t)(int8_t)mem_read8(cpu->eip++);
    uint32_t a = read_rm32(cpu, &mrm);
    uint32_t result = 0;
    switch (mrm.reg) {
        case 0:
            result = a + imm;
            write_rm32(cpu, &mrm, result);
            set_cf(cpu, result < a);
            set_of_add(cpu, a, imm, result);
            break;
        case 1:
            result = a | imm;
            write_rm32(cpu, &mrm, result);
            set_cf(cpu, 0);
            break;
        case 4:
            result = a & imm;
            write_rm32(cpu, &mrm, result);
            set_cf(cpu, 0);
            break;
        case 5:
            result = a - imm;
            write_rm32(cpu, &mrm, result);
            set_cf(cpu, a < imm);
            set_of_sub(cpu, a, imm, result);
            break;
        case 6:
            result = a ^ imm;
            write_rm32(cpu, &mrm, result);
            set_cf(cpu, 0);
            break;
        case 7: // CMP
            result = a - imm;
            set_cf(cpu, a < imm);
            set_of_sub(cpu, a, imm, result);
            break;
        default:
            fprintf(stderr, "unhandled 0x83 reg=%d\n", mrm.reg);
            cpu->halted = 1;
            return;
    }
    set_zf(cpu, result);
    set_sf(cpu, result);
    cpu->cycles += 2;
}

// 0x84 TEST r/m8, r8
static void op_test_rm8_r8(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint8_t a = (mrm.mod == 3)
        ? (uint8_t)cpu->regs[mrm.rm]
        : mem_read8(resolve_rm_addr(cpu, &mrm));
    uint8_t b = (uint8_t)cpu->regs[mrm.reg];
    uint8_t result = a & b;
    set_zf(cpu, (uint32_t)result);
    set_sf(cpu, (uint32_t)result);
    cpu->cycles += 2;
}

// 0x85 TEST r/m32, r32
static void op_test_rm32_r32(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint32_t result = read_rm32(cpu, &mrm) & cpu->regs[mrm.reg];
    set_zf(cpu, result);
    set_sf(cpu, result);
    cpu->cycles += 2;
}

// 0x89 MOV r/m32, r32
static void op_mov_rm32_r32(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    if (g_operand_size_16) {
        uint16_t val = (uint16_t)cpu->regs[mrm.reg];
        if (mrm.mod == 3) cpu->regs[mrm.rm] = (cpu->regs[mrm.rm] & 0xFFFF0000) | val;
        else mem_write16(resolve_rm_addr(cpu, &mrm), val);
    } else {
        write_rm32(cpu, &mrm, cpu->regs[mrm.reg]);
    }
    cpu->cycles += 2;
}

// 0x8B MOV r32, r/m32
static void op_mov_r32_rm32(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    if (g_operand_size_16) {
        uint16_t val = (mrm.mod == 3)
            ? (uint16_t)cpu->regs[mrm.rm]
            : mem_read16(resolve_rm_addr(cpu, &mrm));
        cpu->regs[mrm.reg] = (cpu->regs[mrm.reg] & 0xFFFF0000) | val;
    } else {
        cpu->regs[mrm.reg] = read_rm32(cpu, &mrm);
    }
    cpu->cycles += 2;
}

// 0x8D LEA r32, m
static void op_lea_r32_m(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    cpu->regs[mrm.reg] = resolve_rm_addr(cpu, &mrm);
    cpu->cycles += 1;
}

// 0x90 NOP
static void op_nop(i386 *cpu, uint8_t op) {
    (void)cpu; (void)op;
    cpu->cycles += 1;
}

// 0xB8 to 0xBF MOV r32, imm32
// low 3 bits of the opcode encode the register
static void op_mov_r32_imm32(i386 *cpu, uint8_t op) {
    uint8_t reg = op & 0x07;
    cpu->regs[reg] = mem_read32(cpu->eip);
    cpu->eip += 4;
    cpu->cycles += 1;
}

// 0xC3 RET
static void op_ret(i386 *cpu, uint8_t op) {
    (void)op;
    cpu->eip = mem_read32(cpu->regs[REG_ESP]);
    cpu->regs[REG_ESP] += 4;
    cpu->cycles += 5;
}

// 0xC7 MOV r/m32, imm32
static void op_mov_rm32_imm32(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint32_t imm = mem_read32(cpu->eip);
    cpu->eip += 4;
    write_rm32(cpu, &mrm, imm);
    cpu->cycles += 2;
}

// 0xC9 LEAVE  (mov esp, ebp; pop ebp)
static void op_leave(i386 *cpu, uint8_t op) {
    (void)op;
    cpu->regs[REG_ESP] = cpu->regs[REG_EBP];
    cpu->regs[REG_EBP] = mem_read32(cpu->regs[REG_ESP]);
    cpu->regs[REG_ESP] += 4;
    cpu->cycles += 3;
}

// 0xCD INT imm8
static void op_int(i386 *cpu, uint8_t op) {
    (void)op;
    uint8_t vector = mem_read8(cpu->eip++);
    if (vector == 0x80) dispatch_syscall(cpu);
    else {
        // don't know how to handle any other interrupt vectors yet
        fprintf(stderr, "unhandled interrupt vector 0x%02X\n", vector);
        cpu->halted = 1;
    }
    cpu->cycles += 50;
}

// 0xE8 CALL rel32
static void op_call_rel32(i386 *cpu, uint8_t op) {
    (void)op;
    int32_t offset = (int32_t)mem_read32(cpu->eip);
    cpu->eip += 4;
    // push the return address onto the stack
    cpu->regs[REG_ESP] -= 4;
    mem_write32(cpu->regs[REG_ESP], cpu->eip);
    // jump to the target
    cpu->eip += offset;
    cpu->cycles += 5;
}

// 0xE9 JMP rel32
static void op_jmp_near(i386 *cpu, uint8_t op) {
    (void)op;
    int32_t offset = (int32_t)mem_read32(cpu->eip);
    cpu->eip += 4;
    cpu->eip += offset;
    cpu->cycles += 1;
}

// 0xEB JMP rel8 (short jump)
static void op_jmp_short(i386 *cpu, uint8_t op) {
    (void)op;
    int8_t offset = (int8_t)mem_read8(cpu->eip++);
    cpu->eip += offset;
    cpu->cycles += 1;
}

// 0xF3 REP prefix
static void op_rep(i386 *cpu, uint8_t op) {
    (void)op;
    uint8_t next = mem_read8(cpu->eip++);
    switch (next) {
        case 0xA4: // REP MOVSB - copy ECX bytes from ESI to EDI
            while (cpu->regs[REG_ECX] > 0) {
                mem_write8(cpu->regs[REG_EDI], mem_read8(cpu->regs[REG_ESI]));
                cpu->regs[REG_ESI]++;
                cpu->regs[REG_EDI]++;
                cpu->regs[REG_ECX]--;
            }
            cpu->cycles += cpu->regs[REG_ECX] + 1;
            break;
        case 0xA5: // REP MOVSD - copy ECX dwords from ESI to EDI
            while (cpu->regs[REG_ECX] > 0) {
                mem_write32(cpu->regs[REG_EDI], mem_read32(cpu->regs[REG_ESI]));
                cpu->regs[REG_ESI] += 4;
                cpu->regs[REG_EDI] += 4;
                cpu->regs[REG_ECX]--;
            }
            cpu->cycles += cpu->regs[REG_ECX] + 1;
            break;
        case 0xAA: // REP STOSB - fill ECX bytes at EDI with AL
            while (cpu->regs[REG_ECX] > 0) {
                mem_write8(cpu->regs[REG_EDI], (uint8_t)cpu->regs[REG_EAX]);
                cpu->regs[REG_EDI]++;
                cpu->regs[REG_ECX]--;
            }
            cpu->cycles += cpu->regs[REG_ECX] + 1;
            break;
        case 0xAB: // REP STOSD - fill ECX dwords at EDI with EAX
            while (cpu->regs[REG_ECX] > 0) {
                mem_write32(cpu->regs[REG_EDI], cpu->regs[REG_EAX]);
                cpu->regs[REG_EDI] += 4;
                cpu->regs[REG_ECX]--;
            }
            cpu->cycles += cpu->regs[REG_ECX] + 1;
            break;
        default:
            fprintf(stderr, "unhandled REP instruction 0x%02X at EIP 0x%08X\n",
                    next, cpu->eip - 2);
            cpu->halted = 1;
            break;
    }
}

// 0xFF group
static void op_ff_group(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);

    switch (mrm.reg) {
        case 2: { // CALL r/m32
            uint32_t target = read_rm32(cpu, &mrm);
            cpu->regs[REG_ESP] -= 4;
            mem_write32(cpu->regs[REG_ESP], cpu->eip);
            cpu->eip = target;
            cpu->cycles += 5;
            break;
        }
        case 4: { // JMP r/m32
            cpu->eip = read_rm32(cpu, &mrm);
            cpu->cycles += 3;
            break;
        }
        case 6: { // PUSH r/m32
            uint32_t val = read_rm32(cpu, &mrm);
            cpu->regs[REG_ESP] -= 4;
            mem_write32(cpu->regs[REG_ESP], val);
            cpu->cycles += 2;
            break;
        }
        default:
            fprintf(stderr, "unhandled 0xFF reg=%d at EIP 0x%08X\n", mrm.reg, cpu->eip);
            cpu->halted = 1;
            break;
    }
}

// table init and dispatch
void init_opcode_table(void) {
    for (int i = 0; i < 256; i++) opcode_table[i] = op_unimplemented;
    for (int i = 0; i < 256; i++) opcode_table_0f[i] = op_0f_unimplemented;

    // 0x0F prefix
    opcode_table[0x0F] = op_prefix_0f;
    for (int i = 0x40; i <= 0x4F; i++) opcode_table_0f[i] = op_cmovcc;
    opcode_table_0f[0xAF] = op_imul_r32_rm32;
    for (int i = 0x80; i <= 0x8F; i++) opcode_table_0f[i] = op_jcc_near;
    opcode_table_0f[0xB6] = op_movzx_r32_rm8;

    opcode_table[0x01] = op_add_rm32_r32;
    opcode_table[0x03] = op_add_r32_rm32;
    opcode_table[0x05] = op_add_eax_imm32;
    opcode_table[0x29] = op_sub_rm32_r32;
    opcode_table[0x2B] = op_sub_r32_rm32;
    opcode_table[0x31] = op_xor_rm32_r32;
    opcode_table[0x39] = op_cmp_rm32_r32;
    opcode_table[0x3B] = op_cmp_r32_rm32;
    for (int i = 0x40; i <= 0x47; i++) opcode_table[i] = op_inc_r32;
    for (int i = 0x48; i <= 0x4F; i++) opcode_table[i] = op_dec_r32;
    for (int i = 0x50; i <= 0x57; i++) opcode_table[i] = op_push_r32;
    for (int i = 0x58; i <= 0x5F; i++) opcode_table[i] = op_pop_r32;
    opcode_table[0x65] = op_prefix_gs;
    opcode_table[0x66] = op_prefix_66;
    opcode_table[0x68] = op_push_imm32;
    opcode_table[0x6A] = op_push_imm8;
    for (int i = 0x70; i <= 0x7F; i++) opcode_table[i] = op_jcc_short;
    opcode_table[0x81] = op_81;
    opcode_table[0x83] = op_83;
    opcode_table[0x84] = op_test_rm8_r8;
    opcode_table[0x85] = op_test_rm32_r32;
    opcode_table[0x89] = op_mov_rm32_r32;
    opcode_table[0x8B] = op_mov_r32_rm32;
    opcode_table[0x8D] = op_lea_r32_m;
    opcode_table[0x90] = op_nop;
    for (int i = 0xB8; i <= 0xBF; i++) opcode_table[i] = op_mov_r32_imm32;
    opcode_table[0xC3] = op_ret;
    opcode_table[0xC7] = op_mov_rm32_imm32;
    opcode_table[0xC9] = op_leave;
    opcode_table[0xCD] = op_int;
    opcode_table[0xE8] = op_call_rel32;
    opcode_table[0xE9] = op_jmp_near;
    opcode_table[0xEB] = op_jmp_short;
    opcode_table[0xF3] = op_rep;
    opcode_table[0xFF] = op_ff_group;
}

void execute_opcode(i386 *cpu, uint8_t opcode) {
    opcode_table[opcode](cpu, opcode);
}
