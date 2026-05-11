#include "syscall.h"
#include "../cpu/cpu.h"
#include <stdio.h>

// linux i386 syscall numbers
#define LINUX_SYS_EXIT            1
#define LINUX_SYS_READ            3
#define LINUX_SYS_WRITE           4
#define LINUX_SYS_OPEN            5
#define LINUX_SYS_CLOSE           6
#define LINUX_SYS_LSEEK           19
#define LINUX_SYS_BRK             45
#define LINUX_SYS_SET_THREAD_AREA 243

#define ENOSYS  38

void dispatch_syscall(i386 *cpu) {
    uint32_t nr  = cpu->regs[REG_EAX];
    uint32_t ebx = cpu->regs[REG_EBX];
    uint32_t ecx = cpu->regs[REG_ECX];
    uint32_t edx = cpu->regs[REG_EDX];

    switch (nr) {
        case LINUX_SYS_EXIT:
            sys_exit(cpu, (int)ebx);
            // sys_exit sets halted, nothing to return
            return;

        case LINUX_SYS_READ:
            cpu->regs[REG_EAX] = (uint32_t)sys_read(cpu, (int)ebx, ecx, edx);
            return;

        case LINUX_SYS_WRITE:
            cpu->regs[REG_EAX] = (uint32_t)sys_write(cpu, (int)ebx, ecx, edx);
            return;

        case LINUX_SYS_OPEN:
            cpu->regs[REG_EAX] = (uint32_t)sys_open(cpu, ebx, (int)ecx, (int)edx);
            return;

        case LINUX_SYS_CLOSE:
            cpu->regs[REG_EAX] = (uint32_t)sys_close(cpu, (int)ebx);
            return;

       case LINUX_SYS_LSEEK:
            cpu->regs[REG_EAX] = (uint32_t)sys_lseek(cpu, (int)ebx, (int32_t)ecx, (int)edx);
            return;

        case LINUX_SYS_BRK:
            cpu->regs[REG_EAX] = sys_brk(cpu, ebx);
            return;

        case LINUX_SYS_SET_THREAD_AREA:
            cpu->regs[REG_EAX] = (uint32_t)sys_set_thread_area(cpu, ebx);
            return;

        default:
            fprintf(stderr, "syscall: unimplemented nr=%d\n", nr);
            cpu->regs[REG_EAX] = (uint32_t)(-ENOSYS);
            return;
    }
}
