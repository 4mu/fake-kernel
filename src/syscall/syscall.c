#include "syscall.h"
#include "../cpu/cpu.h"
#include <stdio.h>

// linux i386 syscall numbers
#define SYS_EXIT   1
#define SYS_READ   3
#define SYS_WRITE  4
#define SYS_BRK    45

#define ENOSYS  38

void dispatch_syscall(i386 *cpu) {
    uint32_t nr  = cpu->regs[REG_EAX];
    uint32_t ebx = cpu->regs[REG_EBX];
    uint32_t ecx = cpu->regs[REG_ECX];
    uint32_t edx = cpu->regs[REG_EDX];

    switch (nr) {
        case SYS_EXIT:
            sys_exit(cpu, (int)ebx);
            // sys_exit sets halted, nothing to return
            return;

        case SYS_READ:
            cpu->regs[REG_EAX] = (uint32_t)sys_read(cpu, (int)ebx, ecx, edx);
            return;

        case SYS_WRITE:
            cpu->regs[REG_EAX] = (uint32_t)sys_write(cpu, (int)ebx, ecx, edx);
            return;

        case SYS_BRK:
            cpu->regs[REG_EAX] = sys_brk(cpu, ebx);
            return;

        default:
            fprintf(stderr, "syscall: unimplemented nr=%d\n", nr);
            cpu->regs[REG_EAX] = (uint32_t)(-ENOSYS);
            return;
    }
}
