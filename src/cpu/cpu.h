#ifndef CPU_H
#define CPU_H

#include <stdint.h>

// x86 register indices, these match the actual encoding order in the ISA
// so modrm decoding can just index directly into regs[]
#define REG_EAX 0
#define REG_ECX 1
#define REG_EDX 2
#define REG_EBX 3
#define REG_ESP 4
#define REG_EBP 5
#define REG_ESI 6
#define REG_EDI 7

typedef struct {
    uint32_t regs[8];
    uint32_t eip;
    uint32_t eflags;

    int halted;
    int cycles;
} i386;

// decoded modrm byte, filled in by parse_modrm in ops.c
typedef struct {
    uint8_t mod;
    uint8_t reg; // destination reg or opcode extension depending on the instruction
    uint8_t rm;

    int has_sib;
    int has_disp;
    uint32_t disp; // sign extended to 32 bits
    uint8_t sib_base;
    uint8_t sib_index;
    uint8_t sib_scale;
} ModRM;

void init_cpu(i386 *cpu);
void emulate(i386 *cpu);

void set_gs_base(uint32_t base);
uint32_t get_gs_base(void);

// implemented in ops.c
void execute_opcode(i386 *cpu, uint8_t opcode);

#endif
