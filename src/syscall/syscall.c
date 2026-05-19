#include "syscall.h"
#include "../cpu/cpu.h"
#include "../mem/mem.h"
#include <stdio.h>

// linux i386 syscall numbers
#define LINUX_SYS_EXIT            1
#define LINUX_SYS_READ            3
#define LINUX_SYS_WRITE           4
#define LINUX_SYS_OPEN            5
#define LINUX_SYS_CLOSE           6
#define LINUX_SYS_LSEEK           19
#define LINUX_SYS_GETPID          20
#define LINUX_SYS_KILL            37
#define LINUX_SYS_BRK             45
#define LINUX_SYS_MMAP2           192
#define LINUX_SYS_FUTEX           240
#define LINUX_SYS_SET_THREAD_AREA 243
#define LINUX_SYS_CLOCK_GETTIME   265
#define LINUX_SYS_RSEQ            386

#define ENOSYS  38

void dispatch_syscall(i386 *cpu) {
    uint32_t nr = cpu->regs[REG_EAX];
    uint32_t ebx = cpu->regs[REG_EBX];
    uint32_t ecx = cpu->regs[REG_ECX];
    uint32_t edx = cpu->regs[REG_EDX];

    fprintf(stderr, "syscall nr=%d ebx=0x%08X ecx=0x%08X edx=0x%08X\n", nr, ebx, ecx, edx);

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

        case LINUX_SYS_GETPID:
            cpu->regs[REG_EAX] = 1;
            return;

        case LINUX_SYS_KILL:
            // ignore signals, we're single threaded
            cpu->regs[REG_EAX] = 0;
            return;

        case LINUX_SYS_BRK:
            cpu->regs[REG_EAX] = sys_brk(cpu, ebx);
            return;

        case LINUX_SYS_MMAP2:
            cpu->regs[REG_EAX] = (uint32_t)-ENOSYS;
            return;

        case LINUX_SYS_FUTEX:
            // single threaded so return 0, no waiting needed
            cpu->regs[REG_EAX] = 0;
            return;

        case LINUX_SYS_SET_THREAD_AREA:
            cpu->regs[REG_EAX] = (uint32_t)sys_set_thread_area(cpu, ebx);
            return;

        case LINUX_SYS_CLOCK_GETTIME:
            // fill timespec with zeros, good enough for malloc seeding
            if (cpu->regs[REG_ECX]) {
                mem_write32(cpu->regs[REG_ECX], 0); // tv_sec
                mem_write32(cpu->regs[REG_ECX] + 4, 0); // tv_nsec
            }
            cpu->regs[REG_EAX] = 0;
            return;

        case LINUX_SYS_RSEQ:
            // register restartable sequence, just return 0
            // the rseq struct address is in EBX, we don't actually need to do anything
            cpu->regs[REG_EAX] = 0;
            return;

        default:
            fprintf(stderr, "syscall: unimplemented nr=%d ebx=0x%08X\n", nr, cpu->regs[REG_EBX]);
            cpu->regs[REG_EAX] = (uint32_t)(-ENOSYS);
            return;
    }
    fprintf(stderr, "syscall nr=%d returned 0x%08X\n", nr, cpu->regs[REG_EAX]);
}
