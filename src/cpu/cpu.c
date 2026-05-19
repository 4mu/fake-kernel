#include "cpu.h"
#include "../mem/mem.h"
#include <string.h>
#include <stdio.h>

void init_cpu(i386 *cpu) {
    memset(cpu, 0, sizeof(*cpu));
    // bit 1 of eflags is hardwired to 1 on real x86, always set it
    cpu->eflags = 0x00000002;
}

void emulate(i386 *cpu) {
    while (!cpu->halted) {
        g_last_eip = cpu->eip;
        uint8_t opcode = mem_read8(cpu->eip);
        cpu->eip++;
        execute_opcode(cpu, opcode);
    }
}
