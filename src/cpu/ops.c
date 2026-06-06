#include "cpu.h"
#include "../mem/mem.h"
#include "../syscall/syscall.h"
#include <stdio.h>

#define FLAG_CF (1 << 0)
#define FLAG_PF (1 << 2)
#define FLAG_ZF (1 << 6)
#define FLAG_SF (1 << 7)
#define FLAG_OF (1 << 11)

// not re-entrant, op_prefix_66 sets this before calling execute_opcode
// and clears it after, so nothing else should touch it
static int g_operand_size_16 = 0;
static uint32_t g_gs_base = 0;
static uint32_t g_seg_override = 0;

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

static void set_pf(i386 *cpu, uint32_t result) {
    uint8_t b = (uint8_t)result;
    b ^= b >> 4;
    b ^= b >> 2;
    b ^= b >> 1;
    if (!(b & 1)) cpu->eflags |= FLAG_PF;
    else cpu->eflags &= ~FLAG_PF;
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
static int get_pf(i386 *cpu) { return (cpu->eflags >> 2)  & 1; }
static int get_zf(i386 *cpu) { return (cpu->eflags >> 6)  & 1; }
static int get_sf(i386 *cpu) { return (cpu->eflags >> 7)  & 1; }
static int get_of(i386 *cpu) { return (cpu->eflags >> 11) & 1; }

static inline uint8_t get_reg8(i386 *cpu, int reg) {
    // regs 0-3 = AL CL DL BL (low byte), 4-7 = AH CH DH BH (high byte)
    if (reg < 4)
        return (uint8_t)(cpu->regs[reg]);
    else
        return (uint8_t)(cpu->regs[reg - 4] >> 8);
}

static inline void set_reg8(i386 *cpu, int reg, uint8_t val) {
    if (reg < 4)
        cpu->regs[reg] = (cpu->regs[reg] & 0xFFFFFF00) | val;
    else
        cpu->regs[reg - 4] = (cpu->regs[reg - 4] & 0xFFFF00FF) | ((uint32_t)val << 8);
}

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
            out->sib_scale = (sib >> 6) & 0x03;
            out->sib_index = (sib >> 3) & 0x07;
            out->sib_base  = sib & 0x07;
        }
        if (out->mod == 1) {
            out->has_disp = 1;
            out->disp = (int32_t)((int8_t)mem_read8(cpu->eip++));
        } else if (out->mod == 2) {
            out->has_disp = 1;
            out->disp = mem_read32(cpu->eip);
            cpu->eip += 4;
        } else if (out->mod == 0) {
            // two cases need a disp32 with mod=0:
            // 1. rm=5 means disp32 with no base (no SIB)
            // 2. SIB with base=5 means disp32 with no base register
            if (out->rm == 5 || (out->has_sib && out->sib_base == 5)) {
                out->has_disp = 1;
                out->disp = mem_read32(cpu->eip);
                cpu->eip += 4;
            }
        }
    }
}

static uint32_t resolve_rm_addr(i386 *cpu, ModRM *mrm) {
    if (mrm->mod == 3) return 0;

    uint32_t addr = 0;
    if (mrm->has_sib) {
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
    addr += g_seg_override;
    g_seg_override = 0;
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

// 0x26, 0x2E, 0x36, 0x3E, 0x64 CS segment override prefix, no-op in flat memory model
static void op_prefix_cs(i386 *cpu, uint8_t op) {
    (void)op;
    uint8_t next = mem_read8(cpu->eip++);
    execute_opcode(cpu, next);
    cpu->cycles += 1;
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
        case 0x4A: taken =  get_pf(cpu); break; // CMOVP stub, parity flag not implemented
        case 0x4B: taken = !get_pf(cpu); break; // CMOVNP stub
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

// 0x0F 0x74-0x7D JCC short
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
        case 0x7A: taken =  get_pf(cpu); break; // JP stub, parity flag not implemented
        case 0x7B: taken = !get_pf(cpu); break; // JNP stub
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

// 0x0F 0x9x SETcc r/m8
static void op_setcc(i386 *cpu, uint8_t op) {
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint8_t val = 0;
    switch (op) {
        case 0x90: val =  get_of(cpu); break; // SETO
        case 0x91: val = !get_of(cpu); break; // SETNO
        case 0x92: val =  get_cf(cpu); break; // SETB
        case 0x93: val = !get_cf(cpu); break; // SETNB
        case 0x94: val =  get_zf(cpu); break; // SETE
        case 0x95: val = !get_zf(cpu); break; // SETNE
        case 0x96: val =  get_cf(cpu) || get_zf(cpu);  break; // SETBE
        case 0x97: val = !get_cf(cpu) && !get_zf(cpu); break; // SETA
        case 0x98: val =  get_sf(cpu); break; // SETS
        case 0x99: val = !get_sf(cpu); break; // SETNS
        case 0x9A: val =  get_pf(cpu); break; // SETP stub
        case 0x9B: val = !get_pf(cpu); break; // SETNP stub
        case 0x9C: val =  get_sf(cpu) != get_of(cpu); break; // SETL
        case 0x9D: val =  get_sf(cpu) == get_of(cpu); break; // SETGE
        case 0x9E: val =  get_zf(cpu) || (get_sf(cpu) != get_of(cpu)); break; // SETLE
        case 0x9F: val = !get_zf(cpu) && (get_sf(cpu) == get_of(cpu)); break; // SETG
        default:
            fprintf(stderr, "unhandled SETcc op=0x%02X\n", op);
            cpu->halted = 1;
            return;
    }
    if (mrm.mod == 3)
        set_reg8(cpu, mrm.rm, val);
    else
        mem_write8(resolve_rm_addr(cpu, &mrm), val);
    cpu->cycles += 2;
}

// 0x0F 0xA2 CPUID
static void op_cpuid(i386 *cpu, uint8_t op) {
    (void)op;
    uint32_t leaf = cpu->regs[REG_EAX];

    switch (leaf) {
        case 0: // max leaf + vendor string "GenuineIntel"
            cpu->regs[REG_EAX] = 7;
            cpu->regs[REG_EBX] = 0x756E6547; // "Genu"
            cpu->regs[REG_EDX] = 0x49656E69; // "ineI"
            cpu->regs[REG_ECX] = 0x6C65746E; // "ntel"
            break;

        case 1: // family/model/stepping + feature flags
            cpu->regs[REG_EAX] = 0x00000623; // i686
            cpu->regs[REG_EBX] = 0x00000000;
            cpu->regs[REG_ECX] = 0x00000000; // no SSE3/SSSE3 etc
            // FPU(0) VME(1) DE(2) PSE(3) TSC(4) MSR(5) PAE(6) MCE(7)
            // CX8(8) APIC(9) SEP(11) MTRR(12) PGE(13) MCA(14) CMOV(15)
            // PAT(16) PSE36(17) MMX(23) FXSR(24) SSE(25) SSE2(26)
            cpu->regs[REG_EDX] = 0x0783FBFF;
            break;

        case 2: // cache descriptors, return nothing
            cpu->regs[REG_EAX] = 0;
            cpu->regs[REG_EBX] = 0;
            cpu->regs[REG_ECX] = 0;
            cpu->regs[REG_EDX] = 0;
            break;

        case 7: // structured extended features
            cpu->regs[REG_EAX] = 0; // max sub-leaf
            cpu->regs[REG_EBX] = 0; // no AVX2, BMI etc
            cpu->regs[REG_ECX] = 0;
            cpu->regs[REG_EDX] = 0;
            break;

        case 0x80000000: // max extended leaf
            cpu->regs[REG_EAX] = 0x80000001;
            cpu->regs[REG_EBX] = 0;
            cpu->regs[REG_ECX] = 0;
            cpu->regs[REG_EDX] = 0;
            break;

        case 0x80000001: // extended feature flags
            cpu->regs[REG_EAX] = 0;
            cpu->regs[REG_EBX] = 0;
            cpu->regs[REG_ECX] = 0;
            cpu->regs[REG_EDX] = 0; // no 3DNow, no long mode
            break;

        default:
            cpu->regs[REG_EAX] = 0;
            cpu->regs[REG_EBX] = 0;
            cpu->regs[REG_ECX] = 0;
            cpu->regs[REG_EDX] = 0;
            break;
    }
    cpu->cycles += 10;
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

// 0x0F 0xB1 CMPXCHG r/m32, r32
static void op_cmpxchg_rm32_r32(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint32_t dst = read_rm32(cpu, &mrm);
    uint32_t eax = cpu->regs[REG_EAX];
    uint32_t src = cpu->regs[mrm.reg];
    uint32_t result = eax - dst;
    set_zf(cpu, result);
    set_sf(cpu, result);
    set_cf(cpu, eax < dst);
    set_of_sub(cpu, eax, dst, result);
    if (eax == dst) {
        write_rm32(cpu, &mrm, src);
    } else {
        cpu->regs[REG_EAX] = dst;
    }
    cpu->cycles += 6;
}

// 0x0F 0xB6 MOVZX r32, r/m8
static void op_movzx_r32_rm8(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint8_t val = (mrm.mod == 3)
        ? get_reg8(cpu, mrm.rm)
        : mem_read8(resolve_rm_addr(cpu, &mrm));
    cpu->regs[mrm.reg] = (uint32_t)val;
    cpu->cycles += 2;
}

// 0x0F 0xB7 MOVZX r32, r/m16
static void op_movzx_r32_rm16(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint16_t val = (mrm.mod == 3)
        ? (uint16_t)cpu->regs[mrm.rm]
        : mem_read16(resolve_rm_addr(cpu, &mrm));
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
    set_pf(cpu, (uint32_t)result);
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
    set_pf(cpu, (uint32_t)result);
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
    set_pf(cpu, (uint32_t)result);
    set_of_add(cpu, a, b, result);
    cpu->cycles += 1;
}

// 0x06, 0x16, 0x1E, PUSH
static void op_push_sreg(i386 *cpu, uint8_t op) {
    (void)op;
    // segment registers are irrelevant in flat model, push 0
    cpu->regs[REG_ESP] -= 4;
    mem_write32(cpu->regs[REG_ESP], 0);
    cpu->cycles += 1;
}

// 0x07, 0x17, 0x1F POP
static void op_pop_sreg(i386 *cpu, uint8_t op) {
    (void)op;
    cpu->regs[REG_ESP] += 4; // discard
    cpu->cycles += 1;
}

// 0x09 OR r/m32, r32
static void op_or_rm32_r32(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint32_t result = read_rm32(cpu, &mrm) | cpu->regs[mrm.reg];
    write_rm32(cpu, &mrm, result);
    set_zf(cpu, result);
    set_sf(cpu, result);
    set_pf(cpu, (uint32_t)result);
    set_cf(cpu, 0);
    cpu->cycles += 2;
}

// 0x0B OR r32, r/m32
static void op_or_r32_rm32(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint32_t result = cpu->regs[mrm.reg] | read_rm32(cpu, &mrm);
    cpu->regs[mrm.reg] = result;
    set_zf(cpu, result);
    set_sf(cpu, result);
    set_cf(cpu, 0);
    set_pf(cpu, (uint32_t)result);
    cpu->cycles += 2;
}

// 0x0D OR EAX, imm32
static void op_or_eax_imm32(i386 *cpu, uint8_t op) {
    (void)op;
    uint32_t imm = mem_read32(cpu->eip);
    cpu->eip += 4;
    uint32_t result = cpu->regs[REG_EAX] | imm;
    cpu->regs[REG_EAX] = result;
    set_zf(cpu, result);
    set_sf(cpu, result);
    set_pf(cpu, (uint32_t)result);
    set_cf(cpu, 0);
    cpu->cycles += 1;
}

// 0x11 ADC r/m32, r32
static void op_adc_rm32_r32(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint32_t a = read_rm32(cpu, &mrm);
    uint32_t b = cpu->regs[mrm.reg];
    uint32_t cf = get_cf(cpu);
    uint32_t result = a + b + cf;
    write_rm32(cpu, &mrm, result);
    set_cf(cpu, result < a || (cf && result == a));
    set_zf(cpu, result);
    set_sf(cpu, result);
    set_pf(cpu, (uint32_t)result);
    set_of_add(cpu, a, b, result);
    cpu->cycles += 2;
}

// 0x13 ADC r32, r/m32
static void op_adc_r32_rm32(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint32_t a = cpu->regs[mrm.reg];
    uint32_t b = read_rm32(cpu, &mrm);
    uint32_t cf = get_cf(cpu);
    uint32_t result = a + b + cf;
    cpu->regs[mrm.reg] = result;
    set_cf(cpu, result < a || (cf && result == a));
    set_zf(cpu, result);
    set_sf(cpu, result);
    set_pf(cpu, (uint32_t)result);
    set_of_add(cpu, a, b, result);
    cpu->cycles += 2;
}

// 0x19 SBB r/m32, r32
static void op_sbb_rm32_r32(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint32_t a = read_rm32(cpu, &mrm);
    uint32_t b = cpu->regs[mrm.reg];
    uint32_t cf = get_cf(cpu);
    uint32_t result = a - b - cf;
    write_rm32(cpu, &mrm, result);
    set_cf(cpu, a < b + cf || (cf && b == 0xFFFFFFFF));
    set_zf(cpu, result);
    set_sf(cpu, result);
    set_pf(cpu, (uint32_t)result);
    set_of_sub(cpu, a, b, result);
    cpu->cycles += 2;
}

// 0x1B SBB r32, r/m32
static void op_sbb_r32_rm32(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint32_t a = cpu->regs[mrm.reg];
    uint32_t b = read_rm32(cpu, &mrm);
    uint32_t cf = get_cf(cpu);
    uint32_t result = a - b - cf;
    cpu->regs[mrm.reg] = result;
    set_cf(cpu, a < b + cf || (cf && b == 0xFFFFFFFF));
    set_zf(cpu, result);
    set_sf(cpu, result);
    set_pf(cpu, (uint32_t)result);
    set_of_sub(cpu, a, b, result);
    cpu->cycles += 2;
}

// 0x21 AND r/m32, r32
static void op_and_rm32_r32(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint32_t result = read_rm32(cpu, &mrm) & cpu->regs[mrm.reg];
    write_rm32(cpu, &mrm, result);
    set_zf(cpu, result);
    set_sf(cpu, result);
    set_pf(cpu, (uint32_t)result);
    set_cf(cpu, 0);
    cpu->cycles += 2;
}

// 0x23 AND r32, r/m32
static void op_and_r32_rm32(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint32_t result = cpu->regs[mrm.reg] & read_rm32(cpu, &mrm);
    cpu->regs[mrm.reg] = result;
    set_zf(cpu, result);
    set_sf(cpu, result);
    set_pf(cpu, (uint32_t)result);
    set_cf(cpu, 0);
    cpu->cycles += 2;
}

// 0x25 AND EAX, imm32
static void op_and_eax_imm32(i386 *cpu, uint8_t op) {
    (void)op;
    uint32_t imm = mem_read32(cpu->eip);
    cpu->eip += 4;
    uint32_t result = cpu->regs[REG_EAX] & imm;
    cpu->regs[REG_EAX] = result;
    set_zf(cpu, result);
    set_sf(cpu, result);
    set_pf(cpu, (uint32_t)result);
    set_cf(cpu, 0);
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
    set_pf(cpu, (uint32_t)result);
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
    set_pf(cpu, (uint32_t)result);
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
    set_pf(cpu, (uint32_t)result);
    cpu->cycles += 2;
}

// 0x33 XOR r32, r/m32
static void op_xor_r32_rm32(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint32_t result = cpu->regs[mrm.reg] ^ read_rm32(cpu, &mrm);
    cpu->regs[mrm.reg] = result;
    set_zf(cpu, result);
    set_sf(cpu, result);
    set_pf(cpu, (uint32_t)result);
    set_cf(cpu, 0);
    cpu->cycles += 2;
}

// 0x38 CMP r/m8, r8
static void op_cmp_rm8_r8(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint8_t a = (mrm.mod == 3)
        ? get_reg8(cpu, mrm.rm)
        : mem_read8(resolve_rm_addr(cpu, &mrm));
    uint8_t b = get_reg8(cpu, mrm.reg);
    uint8_t result = a - b;
    set_cf(cpu, a < b);
    set_zf(cpu, (uint32_t)result);
    set_sf(cpu, (uint32_t)result);
    set_pf(cpu, (uint32_t)result);
    set_of_sub(cpu, a, b, result);
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
    set_pf(cpu, (uint32_t)result);
    set_of_sub(cpu, a, b, result);
    cpu->cycles += 2;
}

// 0x3A CMP r8, r/m8
static void op_cmp_r8_rm8(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint8_t a = get_reg8(cpu, mrm.reg);
    uint8_t b = (mrm.mod == 3)
        ? get_reg8(cpu, mrm.rm)
        : mem_read8(resolve_rm_addr(cpu, &mrm));
    uint8_t result = a - b;
    set_cf(cpu, a < b);
    set_zf(cpu, (uint32_t)result);
    set_sf(cpu, (uint32_t)result);
    set_pf(cpu, (uint32_t)result);
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
    set_pf(cpu, (uint32_t)result);
    set_of_sub(cpu, a, b, result);
    cpu->cycles += 2;
}

// 0x3C CMP AL, imm8
static void op_cmp_al_imm8(i386 *cpu, uint8_t op) {
    (void)op;
    uint8_t a = (uint8_t)cpu->regs[REG_EAX];
    uint8_t b = mem_read8(cpu->eip++);
    uint8_t result = a - b;
    set_cf(cpu, a < b);
    set_zf(cpu, (uint32_t)result);
    set_sf(cpu, (uint32_t)result);
    set_pf(cpu, (uint32_t)result);
    set_of_sub(cpu, a, b, result);
    cpu->cycles += 1;
}

// 0x3D CMP EAX, imm32
static void op_cmp_eax_imm32(i386 *cpu, uint8_t op) {
    (void)op;
    uint32_t a = cpu->regs[REG_EAX];
    uint32_t b = mem_read32(cpu->eip);
    cpu->eip += 4;
    uint32_t result = a - b;
    set_cf(cpu, a < b);
    set_zf(cpu, result);
    set_sf(cpu, result);
    set_pf(cpu, (uint32_t)result);
    set_of_sub(cpu, a, b, result);
    cpu->cycles += 1;
}

// 0x40-0x47 INC r32
static void op_inc_r32(i386 *cpu, uint8_t op) {
    uint8_t reg = op & 0x07;
    uint32_t a = cpu->regs[reg];
    uint32_t result = a + 1;
    cpu->regs[reg] = result;
    set_zf(cpu, result);
    set_sf(cpu, result);
    set_pf(cpu, (uint32_t)result);
    set_of_add(cpu, a, 1, result);
    // CF is intentionally not touched, x86 behaviour
    cpu->cycles += 1;
}

// 0x48-0x4F DEC r32
static void op_dec_r32(i386 *cpu, uint8_t op) {
    uint8_t reg = op & 0x07;
    uint32_t a = cpu->regs[reg];
    uint32_t result = a - 1;
    cpu->regs[reg] = result;
    set_zf(cpu, result);
    set_sf(cpu, result);
    set_pf(cpu, (uint32_t)result);
    set_of_sub(cpu, a, 1, result);
    // CF is intentionally not touched, x86 behaviour
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
    g_seg_override = g_gs_base;
    uint8_t next = mem_read8(cpu->eip++);
    execute_opcode(cpu, next);
    g_seg_override = 0;
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

// 0x69 IMUL r32, r/m32, imm32
static void op_imul_r32_rm32_imm32(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    int32_t a = (int32_t)read_rm32(cpu, &mrm);
    int32_t b = (int32_t)mem_read32(cpu->eip);
    cpu->eip += 4;
    int32_t result = a * b;
    cpu->regs[mrm.reg] = (uint32_t)result;
    set_zf(cpu, (uint32_t)result);
    set_sf(cpu, (uint32_t)result);
    cpu->cycles += 10;
}

// 0x6A PUSH imm8
static void op_push_imm8(i386 *cpu, uint8_t op) {
    (void)op;
    int32_t imm = (int32_t)(int8_t)mem_read8(cpu->eip++);
    cpu->regs[REG_ESP] -= 4;
    mem_write32(cpu->regs[REG_ESP], (uint32_t)imm);
    cpu->cycles += 1;
}

// 0x6B IMUL r32, r/m32, imm8
static void op_imul_r32_rm32_imm8(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    int32_t a = (int32_t)read_rm32(cpu, &mrm);
    int32_t b = (int32_t)(int8_t)mem_read8(cpu->eip++);
    int32_t result = a * b;
    cpu->regs[mrm.reg] = (uint32_t)result;
    set_zf(cpu, (uint32_t)result);
    set_sf(cpu, (uint32_t)result);
    cpu->cycles += 10;
}

// 0x80 group: ADD/OR/AND/SUB/XOR/CMP r/m8, imm8
static void op_80_group(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint8_t imm = mem_read8(cpu->eip++);
    uint8_t a = (mrm.mod == 3)
        ? get_reg8(cpu, mrm.rm)
        : mem_read8(resolve_rm_addr(cpu, &mrm));
    uint8_t result = 0;

    switch (mrm.reg) {
        case 0: // ADD
            result = a + imm;
            set_cf(cpu, result < a);
            set_of_add(cpu, a, imm, result);
            break;
        case 1: // OR
            result = a | imm;
            set_cf(cpu, 0);
            break;
        case 2: // ADC
            result = a + imm;
            set_cf(cpu, result < a);
            set_of_add(cpu, a, imm, result);
            break;
        case 3: // SBB
            result = a - imm;
            set_cf(cpu, a < imm);
            set_of_sub(cpu, a, imm, result);
            break;
        case 4: // AND
            result = a & imm;
            set_cf(cpu, 0);
            break;
        case 5: // SUB
            result = a - imm;
            set_cf(cpu, a < imm);
            set_of_sub(cpu, a, imm, result);
            break;
        case 6: // XOR
            result = a ^ imm;
            set_cf(cpu, 0);
            break;
        case 7: // CMP
            result = a - imm;
            set_cf(cpu, a < imm);
            set_of_sub(cpu, a, imm, result);
            set_zf(cpu, (uint32_t)result);
            set_sf(cpu, (uint32_t)result);
            set_pf(cpu, (uint32_t)result);
            cpu->cycles += 2;
            return;
        default:
            fprintf(stderr, "unhandled 0x80 reg=%d at EIP 0x%08X\n", mrm.reg, cpu->eip);
            cpu->halted = 1;
            return;
    }

    if (mrm.mod == 3)
        set_reg8(cpu, mrm.rm, result);
    else
        mem_write8(resolve_rm_addr(cpu, &mrm), result);

    set_zf(cpu, (uint32_t)result);
    set_sf(cpu, (uint32_t)result);
    set_pf(cpu, (uint32_t)result);
    cpu->cycles += 2;
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
        ? get_reg8(cpu, mrm.rm)
        : mem_read8(resolve_rm_addr(cpu, &mrm));
    uint8_t b = get_reg8(cpu, mrm.reg);
    uint8_t result = a & b;
    set_zf(cpu, (uint32_t)result);
    set_sf(cpu, (uint32_t)result);
    set_pf(cpu, (uint32_t)result);
    set_cf(cpu, 0);
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
    set_pf(cpu, (uint32_t)result);
    cpu->cycles += 2;
}

// 0x87 XCHG r/m32, r32
static void op_xchg_rm32_r32(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint32_t a = read_rm32(cpu, &mrm);
    uint32_t b = cpu->regs[mrm.reg];
    write_rm32(cpu, &mrm, b);
    cpu->regs[mrm.reg] = a;
    cpu->cycles += 2;
}

// 0x88 MOV r/m8, r8
static void op_mov_rm8_r8(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint8_t val = get_reg8(cpu, mrm.reg);
    if (mrm.mod == 3) set_reg8(cpu, mrm.rm, val);
    else mem_write8(resolve_rm_addr(cpu, &mrm), val);
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

// 0x8A MOV r8, r/m8
static void op_mov_r8_rm8(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint8_t val = (mrm.mod == 3)
        ? get_reg8(cpu, mrm.rm)
        : mem_read8(resolve_rm_addr(cpu, &mrm));
    set_reg8(cpu, mrm.reg, val);
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

// 0x8E MOV Sreg, r/m16 - segment register write, ignore for flat model
static void op_mov_sreg_rm16(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    // in a flat memory model segment registers don't matter
    // just ignore the write
    cpu->cycles += 2;
}

// 0x90 NOP
static void op_nop(i386 *cpu, uint8_t op) {
    (void)cpu; (void)op;
    cpu->cycles += 1;
}

// 0x91-0x97 XCHG EAX, r32
static void op_xchg_eax_r32(i386 *cpu, uint8_t op) {
    uint8_t reg = op & 0x07;
    uint32_t tmp = cpu->regs[REG_EAX];
    cpu->regs[REG_EAX] = cpu->regs[reg];
    cpu->regs[reg] = tmp;
    cpu->cycles += 2;
}

// 0xA1 MOV EAX, [imm32]
static void op_mov_eax_mem(i386 *cpu, uint8_t op) {
    (void)op;
    uint32_t addr = mem_read32(cpu->eip);
    cpu->eip += 4;
    addr += g_seg_override;
    g_seg_override = 0;
    cpu->regs[REG_EAX] = mem_read32(addr);
    cpu->cycles += 2;
}

// 0xA2 MOV [imm32], AL
static void op_mov_mem_al(i386 *cpu, uint8_t op) {
    (void)op;
    uint32_t addr = mem_read32(cpu->eip);
    cpu->eip += 4;
    addr += g_seg_override;
    g_seg_override = 0;
    mem_write8(addr, (uint8_t)cpu->regs[REG_EAX]);
    cpu->cycles += 2;
}

// 0xA3 MOV [imm32], EAX
static void op_mov_mem_eax(i386 *cpu, uint8_t op) {
    (void)op;
    uint32_t addr = mem_read32(cpu->eip);
    cpu->eip += 4;
    addr += g_seg_override;
    g_seg_override = 0;
    mem_write32(addr, cpu->regs[REG_EAX]);
    cpu->cycles += 2;
}

// 0xA4 MOVSB, move byte from [ESI] to [EDI], advance both by 1
static void op_movsb(i386 *cpu, uint8_t op) {
    (void)op;
    mem_write8(cpu->regs[REG_EDI], mem_read8(cpu->regs[REG_ESI]));
    cpu->regs[REG_ESI]++;
    cpu->regs[REG_EDI]++;
    cpu->cycles += 2;
}

// 0xA5 MOVSD, move dword from [ESI] to [EDI], advance both by 4
static void op_movsd(i386 *cpu, uint8_t op) {
    (void)op;
    mem_write32(cpu->regs[REG_EDI], mem_read32(cpu->regs[REG_ESI]));
    cpu->regs[REG_ESI] += 4;
    cpu->regs[REG_EDI] += 4;
    cpu->cycles += 2;
}

// 0xA6 CMPSB - compare [ESI] with [EDI], advance both
static void op_cmpsb(i386 *cpu, uint8_t op) {
    (void)op;
    uint8_t a = mem_read8(cpu->regs[REG_ESI]++);
    uint8_t b = mem_read8(cpu->regs[REG_EDI]++);
    uint8_t result = a - b;
    set_cf(cpu, a < b);
    set_zf(cpu, (uint32_t)result);
    set_sf(cpu, (uint32_t)result);
    set_of_sub(cpu, a, b, result);
    cpu->cycles += 2;
}

// 0xA8 TEST AL, imm8
static void op_test_al_imm8(i386 *cpu, uint8_t op) {
    (void)op;
    uint8_t imm = mem_read8(cpu->eip++);
    uint8_t result = (uint8_t)cpu->regs[REG_EAX] & imm;
    set_zf(cpu, (uint32_t)result);
    set_sf(cpu, (uint32_t)result);
    set_pf(cpu, (uint32_t)result);
    set_cf(cpu, 0);
    cpu->cycles += 1;
}

// 0xA9 TEST EAX, imm32
static void op_test_eax_imm32(i386 *cpu, uint8_t op) {
    (void)op;
    uint32_t imm = mem_read32(cpu->eip);
    cpu->eip += 4;
    uint32_t result = cpu->regs[REG_EAX] & imm;
    set_zf(cpu, result);
    set_sf(cpu, result);
    set_pf(cpu, (uint32_t)result);
    set_cf(cpu, 0);
    cpu->cycles += 1;
}

// 0xAA STOSB, store AL to [EDI] and increment EDI
static void op_stosb(i386 *cpu, uint8_t op) {
    (void)op;
    mem_write8(cpu->regs[REG_EDI], (uint8_t)cpu->regs[REG_EAX]);
    cpu->regs[REG_EDI]++;
    cpu->cycles += 2;
}

// 0xAB STOSD, store EAX to [EDI] and advance EDI by 4
static void op_stosd(i386 *cpu, uint8_t op) {
    (void)op;
    mem_write32(cpu->regs[REG_EDI], cpu->regs[REG_EAX]);
    cpu->regs[REG_EDI] += 4;
    cpu->cycles += 2;
}

// 0xAE SCASB — compare AL with [EDI], advance EDI
static void op_scasb(i386 *cpu, uint8_t op) {
    (void)op;
    uint8_t a = (uint8_t)cpu->regs[REG_EAX];
    uint8_t b = mem_read8(cpu->regs[REG_EDI]);
    cpu->regs[REG_EDI]++;
    uint8_t result = a - b;
    set_cf(cpu, a < b);
    set_zf(cpu, (uint32_t)result);
    set_sf(cpu, (uint32_t)result);
    set_of_sub(cpu, a, b, result);
    cpu->cycles += 2;
}

// 0xB8 to 0xBF MOV r32, imm32
// low 3 bits of the opcode encode the register
static void op_mov_r32_imm32(i386 *cpu, uint8_t op) {
    uint8_t reg = op & 0x07;
    cpu->regs[reg] = mem_read32(cpu->eip);
    cpu->eip += 4;
    cpu->cycles += 1;
}

// 0xC1 shift group: ROL/ROR/SHL/SHR/SAR r/m32, imm8
static void op_c1_group(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint8_t count = mem_read8(cpu->eip++) & 0x1F; // x86 masks shift count to 5 bits
    uint32_t val = read_rm32(cpu, &mrm);
    uint32_t result = 0;

    switch (mrm.reg) {
        case 0: { // ROL
            result = (val << count) | (val >> (32 - count));
            set_cf(cpu, result & 1);
            break;
        }
        case 1: { // ROR
            result = (val >> count) | (val << (32 - count));
            set_cf(cpu, (result >> 31) & 1);
            break;
        }
        case 4: // SHL/SAL
        case 6: {
            result = val << count;
            set_cf(cpu, count ? (val >> (32 - count)) & 1 : 0);
            set_zf(cpu, result);
            set_sf(cpu, result);
            break;
        }
        case 5: { // SHR
            result = val >> count;
            set_cf(cpu, count ? (val >> (count - 1)) & 1 : 0);
            set_zf(cpu, result);
            set_sf(cpu, result);
            break;
        }
        case 7: { // SAR
            result = (uint32_t)((int32_t)val >> count);
            set_cf(cpu, count ? (val >> (count - 1)) & 1 : 0);
            set_zf(cpu, result);
            set_sf(cpu, result);
            break;
        }
        default:
            fprintf(stderr, "unhandled 0xC1 reg=%d at EIP 0x%08X\n", mrm.reg, cpu->eip);
            cpu->halted = 1;
            return;
    }

    write_rm32(cpu, &mrm, result);
    cpu->cycles += 2;
}

// 0xC3 RET
static void op_ret(i386 *cpu, uint8_t op) {
    (void)op;
    cpu->eip = mem_read32(cpu->regs[REG_ESP]);
    cpu->regs[REG_ESP] += 4;
    cpu->cycles += 5;
}

// 0xC6 MOV r/m8, imm8
static void op_mov_rm8_imm8(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint8_t imm = mem_read8(cpu->eip++);
    if (mrm.mod == 3) set_reg8(cpu, mrm.rm, imm);
    else mem_write8(resolve_rm_addr(cpu, &mrm), imm);
    cpu->cycles += 2;
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
    if (g_operand_size_16) {
        // 16-bit leave: SP = BP, pop BP
        cpu->regs[REG_ESP] = (cpu->regs[REG_ESP] & 0xFFFF0000)
                           | (cpu->regs[REG_EBP] & 0x0000FFFF);
        uint16_t val = mem_read16(cpu->regs[REG_ESP]);
        cpu->regs[REG_ESP] += 2;
        cpu->regs[REG_EBP] = (cpu->regs[REG_EBP] & 0xFFFF0000) | val;
    } else {
        cpu->regs[REG_ESP] = cpu->regs[REG_EBP];
        cpu->regs[REG_EBP] = mem_read32(cpu->regs[REG_ESP]);
        cpu->regs[REG_ESP] += 4;
    }
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

// 0xD1 shift group: ROL/ROR/SHL/SHR/SAR r/m32, 1
static void op_d1_group(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint32_t val = read_rm32(cpu, &mrm);
    uint32_t result = 0;

    switch (mrm.reg) {
        case 0: { // ROL
            result = (val << 1) | (val >> 31);
            set_cf(cpu, result & 1);
            break;
        }
        case 1: { // ROR
            result = (val >> 1) | (val << 31);
            set_cf(cpu, (result >> 31) & 1);
            break;
        }
        case 4:
        case 6: { // SHL/SAL
            set_cf(cpu, (val >> 31) & 1);
            result = val << 1;
            set_zf(cpu, result);
            set_sf(cpu, result);
            break;
        }
        case 5: { // SHR
            set_cf(cpu, val & 1);
            result = val >> 1;
            set_zf(cpu, result);
            set_sf(cpu, result);
            break;
        }
        case 7: { // SAR
            set_cf(cpu, val & 1);
            result = (uint32_t)((int32_t)val >> 1);
            set_zf(cpu, result);
            set_sf(cpu, result);
            break;
        }
        default:
            fprintf(stderr, "unhandled 0xD1 reg=%d at EIP 0x%08X\n", mrm.reg, cpu->eip);
            cpu->halted = 1;
            return;
    }

    write_rm32(cpu, &mrm, result);
    cpu->cycles += 2;
}

// 0xD3 shift group: ROL/ROR/SHL/SHR/SAR r/m32, CL
static void op_d3_group(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    uint8_t count = cpu->regs[REG_ECX] & 0x1F;
    uint32_t val = read_rm32(cpu, &mrm);
    uint32_t result = 0;

    switch (mrm.reg) {
        case 0: { // ROL
            result = (val << count) | (val >> (32 - count));
            set_cf(cpu, result & 1);
            break;
        }
        case 1: { // ROR
            result = (val >> count) | (val << (32 - count));
            set_cf(cpu, (result >> 31) & 1);
            break;
        }
        case 4:
        case 6: { // SHL/SAL
            result = val << count;
            set_cf(cpu, count ? (val >> (32 - count)) & 1 : 0);
            set_zf(cpu, result);
            set_sf(cpu, result);
            break;
        }
        case 5: { // SHR
            result = val >> count;
            set_cf(cpu, count ? (val >> (count - 1)) & 1 : 0);
            set_zf(cpu, result);
            set_sf(cpu, result);
            break;
        }
        case 7: { // SAR
            result = (uint32_t)((int32_t)val >> count);
            set_cf(cpu, count ? (val >> (count - 1)) & 1 : 0);
            set_zf(cpu, result);
            set_sf(cpu, result);
            break;
        }
        default:
            fprintf(stderr, "unhandled 0xD3 reg=%d at EIP 0x%08X\n", mrm.reg, cpu->eip);
            cpu->halted = 1;
            return;
    }

    write_rm32(cpu, &mrm, result);
    cpu->cycles += 2;
}

// 0xE3 JECXZ rel8, jump if ECX is zero
static void op_jecxz(i386 *cpu, uint8_t op) {
    (void)op;
    int8_t offset = (int8_t)mem_read8(cpu->eip++);
    if (cpu->regs[REG_ECX] == 0)
        cpu->eip += offset;
    cpu->cycles += 1;
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

// 0xF0 LOCK prefix, no op since we're single threaded
static void op_prefix_lock(i386 *cpu, uint8_t op) {
    (void)op;
    uint8_t next = mem_read8(cpu->eip++);
    execute_opcode(cpu, next);
    cpu->cycles += 1;
}

// 0xF2 REPNE prefix
static void op_repne(i386 *cpu, uint8_t op) {
    (void)op;
    uint8_t next = mem_read8(cpu->eip++);
    switch (next) {
        case 0xAE: { // REPNE SCASB — used by strlen, strchr
            while (cpu->regs[REG_ECX] > 0) {
                uint8_t a = (uint8_t)cpu->regs[REG_EAX];
                uint8_t b = mem_read8(cpu->regs[REG_EDI]);
                cpu->regs[REG_EDI]++;
                cpu->regs[REG_ECX]--;
                uint8_t result = a - b;
                set_cf(cpu, a < b);
                set_zf(cpu, (uint32_t)result);
                set_sf(cpu, (uint32_t)result);
                set_of_sub(cpu, a, b, result);
                if (get_zf(cpu)) break;
            }
            cpu->cycles += cpu->regs[REG_ECX] + 1;
            break;
        }
        default:
            fprintf(stderr, "unhandled REPNE instruction 0x%02X at EIP 0x%08X\n",
                    next, cpu->eip - 2);
            cpu->halted = 1;
            break;
    }
}

// 0xF3 REP prefix
static void op_rep(i386 *cpu, uint8_t op) {
    (void)op;
    uint8_t next = mem_read8(cpu->eip++);
    switch (next) {
        case 0xA4: {
            uint32_t count = cpu->regs[REG_ECX];
            while (cpu->regs[REG_ECX] > 0) {
                mem_write8(cpu->regs[REG_EDI], mem_read8(cpu->regs[REG_ESI]));
                cpu->regs[REG_ESI]++;
                cpu->regs[REG_EDI]++;
                cpu->regs[REG_ECX]--;
            }
            cpu->cycles += count + 1;
            break;
        }
        case 0xA5:
        {
            uint32_t count = cpu->regs[REG_ECX];
            while (cpu->regs[REG_ECX] > 0) {
                mem_write32(cpu->regs[REG_EDI], mem_read32(cpu->regs[REG_ESI]));
                cpu->regs[REG_ESI] += 4;
                cpu->regs[REG_EDI] += 4;
                cpu->regs[REG_ECX]--;
            }
            cpu->cycles += count + 1;
            break;
        }
        case 0xAA: {
            uint32_t count = cpu->regs[REG_ECX];
            while (cpu->regs[REG_ECX] > 0) {
                mem_write8(cpu->regs[REG_EDI], (uint8_t)cpu->regs[REG_EAX]);
                cpu->regs[REG_EDI]++;
                cpu->regs[REG_ECX]--;
            }
            cpu->cycles += count + 1;
            break;
        }
        case 0xAB: {
            uint32_t count = cpu->regs[REG_ECX];
            while (cpu->regs[REG_ECX] > 0) {
                mem_write32(cpu->regs[REG_EDI], cpu->regs[REG_EAX]);
                cpu->regs[REG_EDI] += 4;
                cpu->regs[REG_ECX]--;
            }
            cpu->cycles += count + 1;
            break;
        }
        default:
            fprintf(stderr, "unhandled REP instruction 0x%02X at EIP 0x%08X\n",
                    next, cpu->eip - 2);
            cpu->halted = 1;
            break;
    }
}

// 0xF4 HLT
static void op_hlt(i386 *cpu, uint8_t op) {
    (void)op;
    fprintf(stderr, "HLT at EIP 0x%08X\n", cpu->eip - 1);
    fprintf(stderr, "  EAX=0x%08X EBX=0x%08X ECX=0x%08X EDX=0x%08X\n",
            cpu->regs[REG_EAX], cpu->regs[REG_EBX],
            cpu->regs[REG_ECX], cpu->regs[REG_EDX]);
    fprintf(stderr, "  ESI=0x%08X EDI=0x%08X EBP=0x%08X ESP=0x%08X\n",
            cpu->regs[REG_ESI], cpu->regs[REG_EDI],
            cpu->regs[REG_EBP], cpu->regs[REG_ESP]);
    fprintf(stderr, "  EFLAGS=0x%08X\n", cpu->eflags);
    cpu->halted = 1;
    cpu->cycles += 1;
}

// 0xF6 group: TEST/NOT/NEG/MUL/IMUL/DIV/IDIV r/m8
static void op_f6_group(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);
    switch (mrm.reg) {
        case 0: // TEST r/m8, imm8
        case 1: {
            uint8_t imm = mem_read8(cpu->eip++);
            uint8_t a = (mrm.mod == 3)
                ? get_reg8(cpu, mrm.rm)
                : mem_read8(resolve_rm_addr(cpu, &mrm));
            uint8_t result = a & imm;
            set_zf(cpu, (uint32_t)result);
            set_sf(cpu, (uint32_t)result);
            set_pf(cpu, (uint32_t)result);
            set_cf(cpu, 0);
            cpu->cycles += 2;
            break;
        }
        case 2: { // NOT r/m8
            if (mrm.mod == 3)
                set_reg8(cpu, mrm.rm, ~get_reg8(cpu, mrm.rm));
            else {
                uint32_t addr = resolve_rm_addr(cpu, &mrm);
                mem_write8(addr, ~mem_read8(addr));
            }
            cpu->cycles += 2;
            break;
        }
        case 3: { // NEG r/m8
            uint8_t val = (mrm.mod == 3)
                ? get_reg8(cpu, mrm.rm)
                : mem_read8(resolve_rm_addr(cpu, &mrm));
            uint8_t result = (uint8_t)(-(int8_t)val);
            if (mrm.mod == 3)
                set_reg8(cpu, mrm.rm, result);
            else
                mem_write8(resolve_rm_addr(cpu, &mrm), result);
            set_cf(cpu, val != 0);
            set_zf(cpu, (uint32_t)result);
            set_sf(cpu, (uint32_t)result);
            cpu->cycles += 2;
            break;
        }
        case 4: { // MUL AX, r/m8
            // AL * r/m8, result goes into AX (full 16 bits)
            uint16_t result = (uint16_t)(cpu->regs[REG_EAX] & 0xFF) *
                              (uint16_t)((mrm.mod == 3)
                                  ? get_reg8(cpu, mrm.rm)
                                  : mem_read8(resolve_rm_addr(cpu, &mrm)));
            cpu->regs[REG_EAX] = (cpu->regs[REG_EAX] & 0xFFFF0000) | result;
            set_cf(cpu, (result >> 8) != 0);
            cpu->cycles += 10;
            break;
        }
        case 5: { // IMUL AX, r/m8
            // same deal, signed, result into AX
            int16_t result = (int16_t)(int8_t)(cpu->regs[REG_EAX] & 0xFF) *
                             (int16_t)(int8_t)((mrm.mod == 3)
                                 ? get_reg8(cpu, mrm.rm)
                                 : mem_read8(resolve_rm_addr(cpu, &mrm)));
            cpu->regs[REG_EAX] = (cpu->regs[REG_EAX] & 0xFFFF0000) | (uint16_t)result;
            set_cf(cpu, (result >> 8) != (result & 0x80 ? -1 : 0));
            cpu->cycles += 10;
            break;
        }
        case 6: { // DIV AX, r/m8
            uint8_t divisor = (mrm.mod == 3)
                ? get_reg8(cpu, mrm.rm)
                : mem_read8(resolve_rm_addr(cpu, &mrm));
            if (divisor == 0) {
                fprintf(stderr, "DIV8 by zero at EIP 0x%08X\n", cpu->eip);
                cpu->halted = 1;
                break;
            }
            uint16_t dividend = (uint16_t)(cpu->regs[REG_EAX] & 0xFFFF);
            // quotient in AL, remainder in AH
            cpu->regs[REG_EAX] = (cpu->regs[REG_EAX] & 0xFFFF0000)
                | ((dividend % divisor) << 8)
                | (dividend / divisor);
            cpu->cycles += 20;
            break;
        }
        case 7: { // IDIV AX, r/m8
            int8_t divisor = (int8_t)((mrm.mod == 3)
                ? get_reg8(cpu, mrm.rm)
                : mem_read8(resolve_rm_addr(cpu, &mrm)));
            if (divisor == 0) {
                fprintf(stderr, "IDIV8 by zero at EIP 0x%08X\n", cpu->eip);
                cpu->halted = 1;
                break;
            }
            int16_t dividend = (int16_t)(cpu->regs[REG_EAX] & 0xFFFF);
            // quotient in AL, remainder in AH
            cpu->regs[REG_EAX] = (cpu->regs[REG_EAX] & 0xFFFF0000)
                | ((uint8_t)(dividend % divisor) << 8)
                | (uint8_t)(dividend / divisor);
            cpu->cycles += 20;
            break;
        }
        default:
            fprintf(stderr, "unhandled 0xF6 reg=%d at EIP 0x%08X\n", mrm.reg, cpu->eip);
            cpu->halted = 1;
            break;
    }
}

// 0xF7 group: TEST/NOT/NEG/MUL/IMUL/DIV/IDIV r/m32
static void op_f7_group(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);

    switch (mrm.reg) {
        case 0: // TEST r/m32, imm32
        case 1: {
            uint32_t imm = mem_read32(cpu->eip);
            cpu->eip += 4;
            uint32_t result = read_rm32(cpu, &mrm) & imm;
            set_zf(cpu, result);
            set_sf(cpu, result);
            set_pf(cpu, (uint32_t)result);
            set_cf(cpu, 0);
            cpu->cycles += 2;
            break;
        }
        case 2: { // NOT r/m32
            write_rm32(cpu, &mrm, ~read_rm32(cpu, &mrm));
            cpu->cycles += 2;
            break;
        }
        case 3: { // NEG r/m32
            uint32_t val = read_rm32(cpu, &mrm);
            uint32_t result = (uint32_t)(-(int32_t)val);
            write_rm32(cpu, &mrm, result);
            set_cf(cpu, val != 0);
            set_zf(cpu, result);
            set_sf(cpu, result);
            set_of_sub(cpu, 0, val, result);
            cpu->cycles += 2;
            break;
        }
        case 4: { // MUL EDX:EAX, r/m32
            uint64_t result = (uint64_t)cpu->regs[REG_EAX] * (uint64_t)read_rm32(cpu, &mrm);
            cpu->regs[REG_EAX] = (uint32_t)(result & 0xFFFFFFFF);
            cpu->regs[REG_EDX] = (uint32_t)(result >> 32);
            set_cf(cpu, cpu->regs[REG_EDX] != 0);
            cpu->cycles += 10;
            break;
        }
        case 5: { // IMUL EDX:EAX, r/m32
            int64_t result = (int64_t)(int32_t)cpu->regs[REG_EAX] * (int64_t)(int32_t)read_rm32(cpu, &mrm);
            cpu->regs[REG_EAX] = (uint32_t)(result & 0xFFFFFFFF);
            cpu->regs[REG_EDX] = (uint32_t)((uint64_t)result >> 32);
            set_cf(cpu, cpu->regs[REG_EDX] != 0);
            cpu->cycles += 10;
            break;
        }
        case 6: { // DIV EDX:EAX, r/m32
            uint32_t divisor = read_rm32(cpu, &mrm);
            if (divisor == 0) {
                // on real x86 this would raise #DE, but for TLS alignment
                // calculations with no TLS a zero divisor means skip it
                fprintf(stderr, "DIV by zero at EIP 0x%08X, skipping\n", cpu->eip);
                cpu->cycles += 20;
                break;
            }
            uint64_t dividend = ((uint64_t)cpu->regs[REG_EDX] << 32) | cpu->regs[REG_EAX];
            cpu->regs[REG_EAX] = (uint32_t)(dividend / divisor);
            cpu->regs[REG_EDX] = (uint32_t)(dividend % divisor);
            cpu->cycles += 20;
            break;
        }
        case 7: { // IDIV EDX:EAX, r/m32
            int32_t divisor = (int32_t)read_rm32(cpu, &mrm);
            if (divisor == 0) {
                fprintf(stderr, "IDIV by zero at EIP 0x%08X\n", cpu->eip);
                cpu->halted = 1;
                break;
            }
            int64_t dividend = (int64_t)(((uint64_t)cpu->regs[REG_EDX] << 32) | cpu->regs[REG_EAX]);
            cpu->regs[REG_EAX] = (uint32_t)(dividend / divisor);
            cpu->regs[REG_EDX] = (uint32_t)(dividend % divisor);
            cpu->cycles += 20;
            break;
        }
        default:
            fprintf(stderr, "unhandled 0xF7 reg=%d at EIP 0x%08X\n", mrm.reg, cpu->eip);
            cpu->halted = 1;
            break;
    }
}

// 0xFC CLD, clears direction flag, REP instructions always go forward so this is a no-op for now
static void op_cld(i386 *cpu, uint8_t op) {
    (void)op;
    cpu->cycles += 1;
}

// 0xFF group
static void op_ff_group(i386 *cpu, uint8_t op) {
    (void)op;
    ModRM mrm;
    parse_modrm(cpu, &mrm);

    switch (mrm.reg) {
        case 2: {
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
    for (int i = 0x80; i <= 0x8F; i++) opcode_table_0f[i] = op_jcc_near;
    for (int i = 0x90; i <= 0x9F; i++) opcode_table_0f[i] = op_setcc;
    opcode_table_0f[0xA2] = op_cpuid;
    opcode_table_0f[0xAF] = op_imul_r32_rm32;
    opcode_table_0f[0xB1] = op_cmpxchg_rm32_r32;
    opcode_table_0f[0xB6] = op_movzx_r32_rm8;
    opcode_table_0f[0xB7] = op_movzx_r32_rm16;

    opcode_table[0x01] = op_add_rm32_r32;
    opcode_table[0x03] = op_add_r32_rm32;
    opcode_table[0x05] = op_add_eax_imm32;
    opcode_table[0x06] = op_push_sreg; // PUSH ES
    opcode_table[0x07] = op_pop_sreg;  // POP ES
    opcode_table[0x09] = op_or_rm32_r32;
    opcode_table[0x0B] = op_or_r32_rm32;
    opcode_table[0x0D] = op_or_eax_imm32;
    opcode_table[0x11] = op_adc_rm32_r32;
    opcode_table[0x13] = op_adc_r32_rm32;
    opcode_table[0x16] = op_push_sreg; // PUSH SS
    opcode_table[0x17] = op_pop_sreg;  // POP SS
    opcode_table[0x19] = op_sbb_rm32_r32;
    opcode_table[0x1B] = op_sbb_r32_rm32;
    opcode_table[0x1E] = op_push_sreg; // PUSH DS
    opcode_table[0x1F] = op_pop_sreg;  // POP DS
    opcode_table[0x21] = op_and_rm32_r32;
    opcode_table[0x23] = op_and_r32_rm32;
    opcode_table[0x25] = op_and_eax_imm32;
    opcode_table[0x26] = op_prefix_cs;
    opcode_table[0x29] = op_sub_rm32_r32;
    opcode_table[0x2B] = op_sub_r32_rm32;
    opcode_table[0x2E] = op_prefix_cs;
    opcode_table[0x31] = op_xor_rm32_r32;
    opcode_table[0x33] = op_xor_r32_rm32;
    opcode_table[0x36] = op_prefix_cs;
    opcode_table[0x38] = op_cmp_rm8_r8;
    opcode_table[0x39] = op_cmp_rm32_r32;
    opcode_table[0x3A] = op_cmp_r8_rm8;
    opcode_table[0x3B] = op_cmp_r32_rm32;
    opcode_table[0x3C] = op_cmp_al_imm8;
    opcode_table[0x3D] = op_cmp_eax_imm32;
    opcode_table[0x3E] = op_prefix_cs;
    for (int i = 0x40; i <= 0x47; i++) opcode_table[i] = op_inc_r32;
    for (int i = 0x48; i <= 0x4F; i++) opcode_table[i] = op_dec_r32;
    for (int i = 0x50; i <= 0x57; i++) opcode_table[i] = op_push_r32;
    for (int i = 0x58; i <= 0x5F; i++) opcode_table[i] = op_pop_r32;
    opcode_table[0x64] = op_prefix_cs;
    opcode_table[0x65] = op_prefix_gs;
    opcode_table[0x66] = op_prefix_66;
    opcode_table[0x68] = op_push_imm32;
    opcode_table[0x69] = op_imul_r32_rm32_imm32;
    opcode_table[0x6A] = op_push_imm8;
    opcode_table[0x6B] = op_imul_r32_rm32_imm8;
    for (int i = 0x70; i <= 0x7F; i++) opcode_table[i] = op_jcc_short;
    opcode_table[0x80] = op_80_group;
    opcode_table[0x81] = op_81;
    opcode_table[0x83] = op_83;
    opcode_table[0x84] = op_test_rm8_r8;
    opcode_table[0x85] = op_test_rm32_r32;
    opcode_table[0x87] = op_xchg_rm32_r32;
    opcode_table[0x88] = op_mov_rm8_r8;
    opcode_table[0x89] = op_mov_rm32_r32;
    opcode_table[0x8A] = op_mov_r8_rm8;
    opcode_table[0x8B] = op_mov_r32_rm32;
    opcode_table[0x8D] = op_lea_r32_m;
    opcode_table[0x8E] = op_mov_sreg_rm16;
    opcode_table[0x90] = op_nop;
    for (int i = 0x91; i <= 0x97; i++) opcode_table[i] = op_xchg_eax_r32;
    opcode_table[0xA1] = op_mov_eax_mem;
    opcode_table[0xA2] = op_mov_mem_al;
    opcode_table[0xA3] = op_mov_mem_eax;
    opcode_table[0xA4] = op_movsb;
    opcode_table[0xA5] = op_movsd;
    opcode_table[0xA6] = op_cmpsb;
    opcode_table[0xA8] = op_test_al_imm8;
    opcode_table[0xA9] = op_test_eax_imm32;
    opcode_table[0xAA] = op_stosb;
    opcode_table[0xAB] = op_stosd;
    opcode_table[0xAE] = op_scasb;
    for (int i = 0xB8; i <= 0xBF; i++) opcode_table[i] = op_mov_r32_imm32;
    opcode_table[0xC1] = op_c1_group;
    opcode_table[0xC3] = op_ret;
    opcode_table[0xC6] = op_mov_rm8_imm8;
    opcode_table[0xC7] = op_mov_rm32_imm32;
    opcode_table[0xC9] = op_leave;
    opcode_table[0xCD] = op_int;
    opcode_table[0xD1] = op_d1_group;
    opcode_table[0xD3] = op_d3_group;
    opcode_table[0xE3] = op_jecxz;
    opcode_table[0xE8] = op_call_rel32;
    opcode_table[0xE9] = op_jmp_near;
    opcode_table[0xEB] = op_jmp_short;
    opcode_table[0xF0] = op_prefix_lock;
    opcode_table[0xF2] = op_repne;
    opcode_table[0xF3] = op_rep;
    opcode_table[0xF4] = op_hlt;
    opcode_table[0xF6] = op_f6_group;
    opcode_table[0xF7] = op_f7_group;
    opcode_table[0xFC] = op_cld;
    opcode_table[0xFF] = op_ff_group;
}

void execute_opcode(i386 *cpu, uint8_t opcode) {
    opcode_table[opcode](cpu, opcode);
}
