#ifndef SYSCALL_H
#define SYSCALL_H

#include "../cpu/cpu.h"

extern uint32_t g_brk;

void init_brk(uint32_t base);
void dispatch_syscall(i386 *cpu);

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

// LINUX_SYS_MUNMAP (91)  ebx=addr  ecx=len
int32_t sys_munmap(i386 *cpu, uint32_t addr, uint32_t len);

// LINUX_SYS_MPROTECT (125)  ebx=addr  ecx=len  edx=prot
int32_t sys_mprotect(i386 *cpu, uint32_t addr, uint32_t len, int prot);

// LINUX_SYS_BRK (45)  ebx=new_brk
uint32_t sys_brk(i386 *cpu, uint32_t new_brk);

// LINUX_SYS_WRITEV (146)  ebx=fd  ecx=iov_addr  edx=iovcnt
int32_t sys_writev(i386 *cpu, int fd, uint32_t iov_addr, int iovcnt);

// LINUX_SYS_RT_SIGACTION (174)  ebx=sig  ecx=act  edx=oact  esi=sigsetsize
int32_t sys_rt_sigaction(i386 *cpu, int sig, uint32_t act_addr, uint32_t oact_addr, uint32_t sigsetsize);

// LINUX_SYS_RT_SIGPROCMASK (175)  ebx=how  ecx=set  edx=oset  esi=sigsetsize
int32_t sys_rt_sigprocmask(i386 *cpu, int how, uint32_t set_addr, uint32_t oset_addr, uint32_t sigsetsize);

// LINUX_SYS_MMAP2 (192)  args in registers: ebx=addr ecx=len edx=prot esi=flags edi=fd ebp=pgoffset
uint32_t sys_mmap2(i386 *cpu, uint32_t addr, uint32_t len, int prot, int flags, int fd, uint32_t pgoffset);

// LINUX_SYS_FSTAT64 (197)  ebx=fd  ecx=statbuf
int32_t sys_fstat64(i386 *cpu, int fd, uint32_t buf_addr);

// LINUX_SYS_SET_THREAD_AREA (243)  ebx=u_info_addr
int32_t sys_set_thread_area(i386 *cpu, uint32_t u_info_addr);

// LINUX_SYS_SET_TID_ADDRESS (258)  ebx=tidptr
int32_t sys_set_tid_address(i386 *cpu, uint32_t tidptr);

#endif
