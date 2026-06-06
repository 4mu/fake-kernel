#include "syscall.h"
#include "../cpu/cpu.h"
#include "../mem/mem.h"
#include <stdio.h>
#include <string.h>

#define LINUX_SYS_EXIT              1
#define LINUX_SYS_READ              3
#define LINUX_SYS_WRITE             4
#define LINUX_SYS_OPEN              5
#define LINUX_SYS_CLOSE             6
#define LINUX_SYS_LSEEK             19
#define LINUX_SYS_GETPID            20
#define LINUX_SYS_KILL              37
#define LINUX_SYS_BRK               45
#define LINUX_SYS_MUNMAP            91
#define LINUX_SYS_MPROTECT          125
#define LINUX_SYS_UNAME             122
#define LINUX_SYS_RT_SIGRETURN      173
#define LINUX_SYS_RT_SIGACTION      174
#define LINUX_SYS_RT_SIGPROCMASK    175
#define LINUX_SYS_MMAP2             192
#define LINUX_SYS_FSTAT64           197
#define LINUX_SYS_FUTEX             240
#define LINUX_SYS_SET_THREAD_AREA   243
#define LINUX_SYS_EXIT_GROUP        252
#define LINUX_SYS_SET_TID_ADDRESS   258
#define LINUX_SYS_CLOCK_GETTIME     265
#define LINUX_SYS_RSEQ              386

#define ENOSYS  38

// uname struct, just the fields glibc checks
typedef struct {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
} LinuxUtsname;

void dispatch_syscall(i386 *cpu) {
    uint32_t nr  = cpu->regs[REG_EAX];
    uint32_t ebx = cpu->regs[REG_EBX];
    uint32_t ecx = cpu->regs[REG_ECX];
    uint32_t edx = cpu->regs[REG_EDX];
    uint32_t esi = cpu->regs[REG_ESI];
    uint32_t edi = cpu->regs[REG_EDI];
    uint32_t ebp = cpu->regs[REG_EBP];

    switch (nr) {
        case LINUX_SYS_EXIT:
            sys_exit(cpu, (int)ebx);
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
            cpu->regs[REG_EAX] = 0;
            return;

        case LINUX_SYS_BRK:
            cpu->regs[REG_EAX] = sys_brk(cpu, ebx);
            return;

        case LINUX_SYS_MUNMAP:
            cpu->regs[REG_EAX] = (uint32_t)sys_munmap(cpu, ebx, ecx);
            return;

        case LINUX_SYS_UNAME: {
            LinuxUtsname *uts = guest_to_host(ebx);
            if (!uts) {
                cpu->regs[REG_EAX] = (uint32_t)(-14); // EFAULT
                return;
            }
            // report as Linux 3.19 i686 so glibc picks the right code paths
            strncpy(uts->sysname,    "Linux",  sizeof(uts->sysname)    - 1);
            strncpy(uts->nodename,   "emu",    sizeof(uts->nodename)   - 1);
            strncpy(uts->release,    "3.19.0", sizeof(uts->release)    - 1);
            strncpy(uts->version,    "#1 SMP", sizeof(uts->version)    - 1);
            strncpy(uts->machine,    "i686",   sizeof(uts->machine)    - 1);
            strncpy(uts->domainname, "(none)", sizeof(uts->domainname) - 1);
            cpu->regs[REG_EAX] = 0;
            return;
        }

        case LINUX_SYS_MPROTECT:
            cpu->regs[REG_EAX] = (uint32_t)sys_mprotect(cpu, ebx, ecx, (int)edx);
            return;

        case LINUX_SYS_RT_SIGRETURN:
            // we never deliver signals so this shouldn't fire, but handle it cleanly
            cpu->regs[REG_EAX] = 0;
            return;

        case LINUX_SYS_RT_SIGACTION:
            cpu->regs[REG_EAX] = (uint32_t)sys_rt_sigaction(cpu, (int)ebx, ecx, edx, esi);
            return;

        case LINUX_SYS_RT_SIGPROCMASK:
            cpu->regs[REG_EAX] = (uint32_t)sys_rt_sigprocmask(cpu, (int)ebx, ecx, edx, esi);
            return;

        case LINUX_SYS_MMAP2:
            // ebx=addr ecx=len edx=prot esi=flags edi=fd ebp=pgoffset
            cpu->regs[REG_EAX] = sys_mmap2(cpu, ebx, ecx, (int)edx, (int)esi, (int)edi, ebp);
            return;

        case LINUX_SYS_FSTAT64:
            cpu->regs[REG_EAX] = (uint32_t)sys_fstat64(cpu, (int)ebx, ecx);
            return;

        case LINUX_SYS_FUTEX:
            // single threaded, nothing actually blocks
            cpu->regs[REG_EAX] = 0;
            return;

        case LINUX_SYS_SET_THREAD_AREA:
            cpu->regs[REG_EAX] = (uint32_t)sys_set_thread_area(cpu, ebx);
            return;

        case LINUX_SYS_EXIT_GROUP:
            sys_exit(cpu, (int)ebx);
            return;

        case LINUX_SYS_SET_TID_ADDRESS:
            cpu->regs[REG_EAX] = (uint32_t)sys_set_tid_address(cpu, ebx);
            return;

        case LINUX_SYS_CLOCK_GETTIME:
            if (ecx) {
                mem_write32(ecx, 0);
                mem_write32(ecx + 4, 0);
            }
            cpu->regs[REG_EAX] = 0;
            return;

        case LINUX_SYS_RSEQ:
            cpu->regs[REG_EAX] = 0;
            return;

        // --- stubs for glibc startup calls we don't need to fully implement ---

        case 146: // writev
            cpu->regs[REG_EAX] = (uint32_t)sys_writev(cpu, (int)ebx, ecx, (int)edx);
            return;

        case 172: // prctl
            cpu->regs[REG_EAX] = 0;
            return;

        case 180: // pread64
            cpu->regs[REG_EAX] = (uint32_t)(-ENOSYS);
            return;

        case 195: // stat64 - glibc probes /etc/ld.so.cache and friends
            cpu->regs[REG_EAX] = (uint32_t)(-2); // ENOENT
            return;

        case 196: // lstat64
            cpu->regs[REG_EAX] = (uint32_t)(-2); // ENOENT
            return;

        case 199: // getuid32
            cpu->regs[REG_EAX] = 1000;
            return;

        case 200: // getgid32
            cpu->regs[REG_EAX] = 1000;
            return;

        case 201: // geteuid32
            cpu->regs[REG_EAX] = 1000;
            return;

        case 202: // getegid32
            cpu->regs[REG_EAX] = 1000;
            return;

        case 219: // madvise
            cpu->regs[REG_EAX] = 0;
            return;

        case 224: // gettid
            cpu->regs[REG_EAX] = 1;
            return;

        case 270: // tgkill
            fprintf(stderr, "tgkill: sig=%d\n", (int)cpu->regs[REG_EDX]);
            if ((int)cpu->regs[REG_EDX] == 6) // SIGABRT
                sys_exit(cpu, 1);
            cpu->regs[REG_EAX] = 0;
            return;

        case 295: // openat: ebx=dirfd ecx=path edx=flags esi=mode
            cpu->regs[REG_EAX] = (uint32_t)sys_open(cpu, ecx, (int)edx, (int)esi);
            return;

        default:
            fprintf(stderr, "syscall: unimplemented nr=%u ebx=0x%08X ecx=0x%08X\n",
                    nr, ebx, ecx);
            g_trace = 1;
            cpu->regs[REG_EAX] = (uint32_t)(-ENOSYS);
            return;
    }
}
