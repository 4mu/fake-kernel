#include "cpu.h"
#include "../mem/mem.h"
#include <string.h>
#include <stdio.h>

void init_cpu(i386 *cpu) {
    memset(cpu, 0, sizeof(*cpu));
    // Bit 1 of EFLAGS is strictly reserved and always 1 in x86
    cpu->eflags = 0x00000002;
}

void emulate(i386 *cpu) {
    while (!cpu->halted) {
        // 1. Fetch
        uint8_t opcode = mem_read8(cpu->eip);
        cpu->eip++;

        // 2 & 3. Decode and Execute
        // We pass the opcode into the dispatcher. The dispatcher will
        // advance EIP further if the instruction has ModR/M or immediate bytes.
        execute_opcode(cpu, opcode);
    }
}
