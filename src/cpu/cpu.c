#include "cpu.h"
#include "../mem/mem.h"
#include <string.h>
#include <stdio.h>

int g_trace = 0;

void init_cpu(i386 *cpu) {
    memset(cpu, 0, sizeof(*cpu));
    // bit 1 of eflags is hardwired to 1 on real x86, always set it
    cpu->eflags = 0x00000002;
}

void emulate(i386 *cpu) {
    while (!cpu->halted) {
        g_last_eip = cpu->eip;
        uint8_t opcode = mem_read8(cpu->eip);

        if (g_trace)
            fprintf(stderr, "EIP=0x%08X op=0x%02X EAX=0x%08X EBX=0x%08X ECX=0x%08X EDX=0x%08X ESP=0x%08X\n",
                    cpu->eip, opcode,
                    cpu->regs[REG_EAX], cpu->regs[REG_EBX],
                    cpu->regs[REG_ECX], cpu->regs[REG_EDX],
                    cpu->regs[REG_ESP]);

        cpu->eip++;
        execute_opcode(cpu, opcode);
    }
}
