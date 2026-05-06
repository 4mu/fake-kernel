#ifndef CPU_H
#define CPU_H

#include <stdint.h>

// i386 Register Indices (Standard x86 encoding order)
#define REG_EAX 0
#define REG_ECX 1
#define REG_EDX 2
#define REG_EBX 3
#define REG_ESP 4
#define REG_EBP 5
#define REG_ESI 6
#define REG_EDI 7

// CPU State
typedef struct {
    uint32_t regs[8];
    uint32_t eip;
    uint32_t eflags;

    int halted;
    int cycles;
} i386;

// Parsed ModR/M Data
typedef struct {
    uint8_t mod;
    uint8_t reg; // Often the destination register, or an opcode extension
    uint8_t rm;  // Register or Memory operand

    // For memory addressing
    int has_sib;
    int has_disp;
    uint32_t disp; // Displacement (8-bit or 32-bit sign-extended)
} ModRM;

// Core CPU interfaces
void init_cpu(i386 *cpu);
void emulate(i386 *cpu);

// Instruction Execution interface (implemented in ops.c)
void execute_opcode(i386 *cpu, uint8_t opcode);

#endif
