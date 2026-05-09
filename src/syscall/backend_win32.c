#include "syscall.h"
#include "../mem/mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

// compiles on both windows and linux so you can test without a windows box
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <unistd.h>
  #include <errno.h>
#endif

// mingw and glibc may already define these so guard them
// we always want the real linux values since these go back to the guest
#ifndef EBADF
#define EBADF    9
#endif
#ifndef EFAULT
#define EFAULT  14
#endif
#ifndef EINVAL
#define EINVAL  22
#endif
#ifndef ENOSYS
#define ENOSYS  38
#endif

#ifdef _WIN32
// maps linux stdin/stdout/stderr fd numbers to win32 handles
static HANDLE get_win32_handle(int fd) {
    switch (fd) {
        case 0: return GetStdHandle(STD_INPUT_HANDLE);
        case 1: return GetStdHandle(STD_OUTPUT_HANDLE);
        case 2: return GetStdHandle(STD_ERROR_HANDLE);
        default: return INVALID_HANDLE_VALUE;
    }
}
#endif

void sys_exit(i386 *cpu, int status) {
    fprintf(stderr, "sys_exit: guest exited with %d\n", status);
    cpu->halted = 1;
    (void)status;
}

int32_t sys_write(i386 *cpu, int fd, uint32_t buf_addr, uint32_t count) {
    (void)cpu;

    if (count == 0) return 0;

    void *host_buf = guest_to_host(buf_addr);
    if (!host_buf) return -EFAULT;

#ifdef _WIN32
    HANDLE h = get_win32_handle(fd);
    if (h == INVALID_HANDLE_VALUE) return -EBADF;

    DWORD written = 0;
    if (!WriteFile(h, host_buf, (DWORD)count, &written, NULL)) {
        fprintf(stderr, "sys_write: WriteFile failed (%lu)\n", GetLastError());
        return -5; // EIO
    }
    return (int32_t)written;
#else
    ssize_t n = write(fd, host_buf, (size_t)count);
    if (n < 0) return -(int32_t)errno;
    return (int32_t)n;
#endif
}

int32_t sys_read(i386 *cpu, int fd, uint32_t buf_addr, uint32_t count) {
    (void)cpu;

    if (count == 0) return 0;

    void *host_buf = guest_to_host(buf_addr);
    if (!host_buf) return -EFAULT;

#ifdef _WIN32
    HANDLE h = get_win32_handle(fd);
    if (h == INVALID_HANDLE_VALUE) return -EBADF;

    DWORD nread = 0;
    if (!ReadFile(h, host_buf, (DWORD)count, &nread, NULL)) {
        DWORD err = GetLastError();
        // broken pipe on a console just means EOF
        if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF) return 0;
        fprintf(stderr, "sys_read: ReadFile failed (%lu)\n", err);
        return -5; // EIO
    }
    return (int32_t)nread;
#else
    ssize_t n = read(fd, host_buf, (size_t)count);
    if (n < 0) return -(int32_t)errno;
    return (int32_t)n;
#endif
}

// simple bump allocator for brk
// ideally this should start right after the last loaded segment but
// we don't track that yet so just hardcode a base above the normal ELF load area
#define HEAP_BASE  0x08100000u

static uint32_t g_brk = 0;

uint32_t sys_brk(i386 *cpu, uint32_t new_brk) {
    (void)cpu;

    if (g_brk == 0) g_brk = HEAP_BASE;

    // new_brk = 0 means the guest just wants to know where the break is
    if (new_brk == 0) return g_brk;

    if (new_brk < HEAP_BASE) return g_brk;

    if ((size_t)new_brk >= mem_size()) {
        fprintf(stderr, "sys_brk: 0x%08X exceeds guest memory\n", new_brk);
        return g_brk;
    }

    g_brk = new_brk;
    return g_brk;
}
