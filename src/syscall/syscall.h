#ifndef SYSCALL_H
#define SYSCALL_H

#include "../cpu/cpu.h"

void init_brk(uint32_t base);
void dispatch_syscall(i386 *cpu);

// called once at startup before emulation begins
void init_fd_table(void);

// LINUX_SYS_EXIT (1)
void sys_exit(i386 *cpu, int status);

// LINUX_SYS_READ (3)  ebx=fd  ecx=buf  edx=count
int32_t sys_read(i386 *cpu, int fd, uint32_t buf_addr, uint32_t count);

// LINUX_SYS_WRITE (4)  ebx=fd  ecx=buf  edx=count
int32_t sys_write(i386 *cpu, int fd, uint32_t buf_addr, uint32_t count);

// LINUX_SYS_OPEN (5)  ebx=path  ecx=flags  edx=mode
int32_t sys_open(i386 *cpu, uint32_t path_addr, int flags, int mode);

// LINUX_SYS_CLOSE (6)  ebx=fd
int32_t sys_close(i386 *cpu, int fd);

// LINUX_SYS_LSEEK (19)  ebx=fd  ecx=offset  edx=whence
int32_t sys_lseek(i386 *cpu, int fd, int32_t offset, int whence);

// LINUX_SYS_BRK (45)  ebx=new_brk
uint32_t sys_brk(i386 *cpu, uint32_t new_brk);

// LINUX_SYS_SET_THREAD_AREA (243)  ebx=u_info_addr
int32_t sys_set_thread_area(i386 *cpu, uint32_t u_info_addr);

#endif
