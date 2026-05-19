#include "syscall.h"
#include "../mem/mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
#endif

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
#ifndef ENOENT
#define ENOENT   2
#endif
#ifndef EACCES
#define EACCES  13
#endif
#ifndef EMFILE
#define EMFILE  24
#endif

// linux open flags
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0x40
#define O_TRUNC   0x200
#define O_APPEND  0x400

#define MAX_FDS 256

typedef struct {
    int used;
#ifdef _WIN32
    HANDLE handle;
#else
    int host_fd;
#endif
} GuestFd;

// linux user_desc struct, used by set_thread_area
typedef struct {
    uint32_t entry_number;
    uint32_t base_addr;
    uint32_t limit;
    uint32_t flags; // packed bitfields, we don't need to unpack them
} UserDesc;

static GuestFd fd_table[MAX_FDS];

void init_fd_table(void) {
    memset(fd_table, 0, sizeof(fd_table));
#ifdef _WIN32
    fd_table[0].used = 1; fd_table[0].handle = GetStdHandle(STD_INPUT_HANDLE);
    fd_table[1].used = 1; fd_table[1].handle = GetStdHandle(STD_OUTPUT_HANDLE);
    fd_table[2].used = 1; fd_table[2].handle = GetStdHandle(STD_ERROR_HANDLE);
#else
    fd_table[0].used = 1; fd_table[0].host_fd = 0;
    fd_table[1].used = 1; fd_table[1].host_fd = 1;
    fd_table[2].used = 1; fd_table[2].host_fd = 2;
#endif
}

static int alloc_fd(void) {
    for (int i = 3; i < MAX_FDS; i++) {
        if (!fd_table[i].used) return i;
    }
    return -1;
}

void sys_exit(i386 *cpu, int status) {
    fprintf(stderr, "sys_exit: guest exited with %d\n", status);
    cpu->halted = 1;
    exit(status);
}

int32_t sys_write(i386 *cpu, int fd, uint32_t buf_addr, uint32_t count) {
    (void)cpu;
    if (count == 0) return 0;
    if (fd < 0 || fd >= MAX_FDS || !fd_table[fd].used) return -EBADF;

    void *host_buf = guest_to_host(buf_addr);
    if (!host_buf) return -EFAULT;

#ifdef _WIN32
    DWORD written = 0;
    if (!WriteFile(fd_table[fd].handle, host_buf, (DWORD)count, &written, NULL)) {
        fprintf(stderr, "sys_write: WriteFile failed (%lu)\n", GetLastError());
        return -5; // EIO
    }
    return (int32_t)written;
#else
    ssize_t n = write(fd_table[fd].host_fd, host_buf, (size_t)count);
    if (n < 0) return -(int32_t)errno;
    return (int32_t)n;
#endif
}

int32_t sys_read(i386 *cpu, int fd, uint32_t buf_addr, uint32_t count) {
    (void)cpu;
    if (count == 0) return 0;
    if (fd < 0 || fd >= MAX_FDS || !fd_table[fd].used) return -EBADF;

    void *host_buf = guest_to_host(buf_addr);
    if (!host_buf) return -EFAULT;

#ifdef _WIN32
    DWORD nread = 0;
    if (!ReadFile(fd_table[fd].handle, host_buf, (DWORD)count, &nread, NULL)) {
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF) return 0;
        fprintf(stderr, "sys_read: ReadFile failed (%lu)\n", err);
        return -5; // EIO
    }
    return (int32_t)nread;
#else
    ssize_t n = read(fd_table[fd].host_fd, host_buf, (size_t)count);
    if (n < 0) return -(int32_t)errno;
    return (int32_t)n;
#endif
}

int32_t sys_open(i386 *cpu, uint32_t path_addr, int flags, int mode) {
    (void)cpu; (void)mode;

    char *path = guest_to_host(path_addr);
    if (!path) return -EFAULT;

    int guest_fd = alloc_fd();
    if (guest_fd < 0) return -EMFILE;

#ifdef _WIN32
    DWORD access = 0;
    if (flags & O_RDWR) access = GENERIC_READ | GENERIC_WRITE;
    else if (flags & O_WRONLY) access = GENERIC_WRITE;
    else access = GENERIC_READ;

    DWORD creation = OPEN_EXISTING;
    if (flags & O_CREAT) creation = (flags & O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS;
    else if (flags & O_TRUNC) creation = TRUNCATE_EXISTING;

    HANDLE h = CreateFileA(path, access, FILE_SHARE_READ, NULL, creation, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) return -ENOENT;
        if (err == ERROR_ACCESS_DENIED) return -EACCES;
        return -5; // EIO
    }

    fd_table[guest_fd].used   = 1;
    fd_table[guest_fd].handle = h;
#else
    int host_fd = open(path, flags, mode);
    if (host_fd < 0) return -(int32_t)errno;
    fd_table[guest_fd].used = 1;
    fd_table[guest_fd].host_fd = host_fd;
#endif

    return guest_fd;
}

int32_t sys_close(i386 *cpu, int fd) {
    (void)cpu;
    if (fd < 0 || fd >= MAX_FDS || !fd_table[fd].used) return -EBADF;
    if (fd <= 2) return 0;

#ifdef _WIN32
    CloseHandle(fd_table[fd].handle);
#else
    close(fd_table[fd].host_fd);
#endif

    fd_table[fd].used = 0;
    return 0;
}

int32_t sys_lseek(i386 *cpu, int fd, int32_t offset, int whence) {
    (void)cpu;
    if (fd < 0 || fd >= MAX_FDS || !fd_table[fd].used) return -EBADF;

#ifdef _WIN32
    DWORD method;
    switch (whence) {
        case 0: method = FILE_BEGIN; break; // SEEK_SET
        case 1: method = FILE_CURRENT; break; // SEEK_CUR
        case 2: method = FILE_END; break; // SEEK_END
        default: return -EINVAL;
    }
    DWORD result = SetFilePointer(fd_table[fd].handle, (LONG)offset, NULL, method);
    if (result == INVALID_SET_FILE_POINTER) return -5; // EIO
    return (int32_t)result;
#else
    off_t result = lseek(fd_table[fd].host_fd, (off_t)offset, whence);
    if (result < 0) return -(int32_t)errno;
    return (int32_t)result;
#endif
}

uint32_t g_brk = 0;

void init_brk(uint32_t base) {
    g_brk = base;
    printf("brk: heap base at 0x%08X\n", g_brk);
}

uint32_t sys_brk(i386 *cpu, uint32_t new_brk) {
    (void)cpu;
    fprintf(stderr, "brk: request=0x%08X current=0x%08X\n", new_brk, g_brk); //temp
    if (new_brk == 0) return g_brk;
    if (new_brk < g_brk) return g_brk;
    if ((size_t)new_brk >= mem_size()) {
        fprintf(stderr, "sys_brk: 0x%08X exceeds guest memory\n", new_brk);
        return g_brk;
    }
    g_brk = new_brk;
    return g_brk;
}

int32_t sys_set_thread_area(i386 *cpu, uint32_t u_info_addr) {
    (void)cpu;
    UserDesc *desc = guest_to_host(u_info_addr);
    if (!desc) return -EFAULT;
    if (desc->entry_number == (uint32_t)-1) desc->entry_number = 0;
    set_gs_base(desc->base_addr);

    // fill positive offsets with stub pointers
    for (int i = 0; i < 0x400; i += 4)
        mem_write32(desc->base_addr + i, 0x00001000);

    // zero negative offsets
    for (int i = 4; i <= 0x400; i += 4)
        mem_write32(desc->base_addr - i, 0);

    mem_write32(desc->base_addr + 0x14, 0xDEADBEEF);

    printf("set_thread_area: GS base set to 0x%08X\n", desc->base_addr);
    return 0;
}
