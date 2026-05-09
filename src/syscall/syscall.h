#ifndef SYSCALL_H
#define SYSCALL_H

#include "../cpu/cpu.h"

// called from the INT 0x80 handler in ops.c
// reads syscall number from EAX, dispatches, writes return value back to EAX
void dispatch_syscall(i386 *cpu);

// backend functions, implemented in backend_win32.c
// args follow the linux i386 ABI: ebx, ecx, edx, esi, edi, ebp
// return value is negative errno on error, same as linux

// SYS_EXIT (1)
void sys_exit(i386 *cpu, int status);

// SYS_WRITE (4)  ebx=fd  ecx=buf  edx=count
int32_t sys_write(i386 *cpu, int fd, uint32_t buf_addr, uint32_t count);

// SYS_READ (3)  ebx=fd  ecx=buf  edx=count
int32_t sys_read(i386 *cpu, int fd, uint32_t buf_addr, uint32_t count);

// SYS_BRK (45)  ebx=new brk, pass 0 to just query the current break
uint32_t sys_brk(i386 *cpu, uint32_t new_brk);

#endif
