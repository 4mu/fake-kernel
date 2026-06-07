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
  #include <sys/stat.h>
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
#ifndef ENOMEM
#define ENOMEM  12
#endif

// linux open flags
#define LINUX_O_RDONLY  0
#define LINUX_O_WRONLY  1
#define LINUX_O_RDWR    2
#define LINUX_O_CREAT   0x40
#define LINUX_O_TRUNC   0x200
#define LINUX_O_APPEND  0x400

// mmap flags we care about
#define LINUX_MAP_SHARED    0x01
#define LINUX_MAP_PRIVATE   0x02
#define LINUX_MAP_FIXED     0x10
#define LINUX_MAP_ANONYMOUS 0x20

#define MAX_FDS 256

typedef struct {
    int used;
#ifdef _WIN32
    HANDLE handle;
#else
    int host_fd;
#endif
} GuestFd;

typedef struct {
    uint32_t iov_base;
    uint32_t iov_len;
} LinuxIovec;

typedef struct {
    uint32_t entry_number;
    uint32_t base_addr;
    uint32_t limit;
    uint32_t flags;
} UserDesc;

// the linux stat64 struct for i386, matches what glibc expects from fstat64
// we only fill in what's needed for glibc's startup checks
typedef struct {
    uint64_t st_dev;
    uint8_t  __pad0[4];
    uint32_t __st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint8_t  __pad3[4];
    int64_t  st_size;
    uint32_t st_blksize;
    uint64_t st_blocks;
    uint32_t st_atime;
    uint32_t st_atime_nsec;
    uint32_t st_mtime;
    uint32_t st_mtime_nsec;
    uint32_t st_ctime;
    uint32_t st_ctime_nsec;
    uint64_t st_ino;
} LinuxStat64;

static GuestFd fd_table[MAX_FDS];

// we manage a simple bump allocator inside the guest memory for mmap
// it lives above the brk heap and below the stack
// mmap2 allocations are never actually freed (munmap is a no-op for anon memory)
// for file-backed mmaps this is a bigger problem but glibc startup only needs anon
static uint32_t g_mmap_base = 0;
static uint32_t g_mmap_ptr  = 0;

// called from main.c after brk is set up, so we can figure out where to put mmap'd memory
// we put the mmap arena in the middle of whatever's left between the brk heap and the stack
void init_mmap_arena(uint32_t mmap_base) {
    g_mmap_base = mmap_base;
    g_mmap_ptr  = mmap_base;
    printf("mmap: arena starts at 0x%08X\n", g_mmap_base);
}

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
    printf("total cycles: %d", cpu->cycles);
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
        return -5;
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
        return -5;
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
    if (flags & LINUX_O_RDWR) access = GENERIC_READ | GENERIC_WRITE;
    else if (flags & LINUX_O_WRONLY) access = GENERIC_WRITE;
    else access = GENERIC_READ;

    DWORD creation = OPEN_EXISTING;
    if (flags & LINUX_O_CREAT) creation = (flags & LINUX_O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS;
    else if (flags & LINUX_O_TRUNC) creation = TRUNCATE_EXISTING;

    HANDLE h = CreateFileA(path, access, FILE_SHARE_READ, NULL, creation, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) return -ENOENT;
        if (err == ERROR_ACCESS_DENIED) return -EACCES;
        return -5;
    }

    fd_table[guest_fd].used   = 1;
    fd_table[guest_fd].handle = h;
#else
    // remap linux open flags to host flags
    int host_flags = 0;
    if ((flags & 3) == LINUX_O_RDWR)   host_flags = O_RDWR;
    else if ((flags & 3) == LINUX_O_WRONLY) host_flags = O_WRONLY;
    else                               host_flags = O_RDONLY;
    if (flags & LINUX_O_CREAT)  host_flags |= O_CREAT;
    if (flags & LINUX_O_TRUNC)  host_flags |= O_TRUNC;
    if (flags & LINUX_O_APPEND) host_flags |= O_APPEND;

    int host_fd = open(path, host_flags, mode);
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
        case 0: method = FILE_BEGIN;   break;
        case 1: method = FILE_CURRENT; break;
        case 2: method = FILE_END;     break;
        default: return -EINVAL;
    }
    DWORD result = SetFilePointer(fd_table[fd].handle, (LONG)offset, NULL, method);
    if (result == INVALID_SET_FILE_POINTER) return -5;
    return (int32_t)result;
#else
    off_t result = lseek(fd_table[fd].host_fd, (off_t)offset, whence);
    if (result < 0) return -(int32_t)errno;
    return (int32_t)result;
#endif
}

int32_t sys_munmap(i386 *cpu, uint32_t addr, uint32_t len) {
    (void)cpu;
    // for anonymous mappings we just leave the guest memory as is,
    // zeroing it out would be more correct but nothing should rely on it
    // for file-backed mappings we'd need to track them, but glibc startup doesn't do that
    (void)addr; (void)len;
    return 0;
}

int32_t sys_mprotect(i386 *cpu, uint32_t addr, uint32_t len, int prot) {
    (void)cpu; (void)addr; (void)len; (void)prot;
    // we don't enforce memory protection, just return success
    return 0;
}

// mmap2 allocates out of the arena above the heap
// we only support anonymous private mappings right now, which is all glibc startup needs
// MAP_FIXED is handled by returning the requested address if it's in our arena
uint32_t sys_mmap2(i386 *cpu, uint32_t addr, uint32_t len, int prot, int flags, int fd, uint32_t pgoffset) {
    (void)cpu; (void)prot; (void)pgoffset;

    if (len == 0) return (uint32_t)-EINVAL;

    // page align the length
    uint32_t aligned_len = (len + 0xFFF) & ~0xFFFu;

    // file-backed mmap: we don't support this yet
    // glibc uses it for loading shared libs, but for static binaries it's not needed
    if (fd != -1 && !(flags & LINUX_MAP_ANONYMOUS)) {
        fprintf(stderr, "mmap2: file-backed mmap not supported (fd=%d len=0x%X)\n", fd, len);
        return (uint32_t)-ENOSYS;
    }

    uint32_t result_addr;

    if (flags & LINUX_MAP_FIXED) {
        // caller specified an address, use it directly
        // just make sure it's within our guest memory
        if ((size_t)addr + aligned_len > mem_size()) {
            fprintf(stderr, "mmap2: MAP_FIXED addr=0x%08X too large\n", addr);
            return (uint32_t)-ENOMEM;
        }
        result_addr = addr;
    } else {
        // allocate from the arena
        if (g_mmap_ptr == 0) {
            fprintf(stderr, "mmap2: arena not initialized, call init_mmap_arena first\n");
            return (uint32_t)-ENOMEM;
        }

        // grow downward from the stack to keep plenty of room
        // but our arena grows up from g_mmap_base for simplicity
        if ((size_t)(g_mmap_ptr + aligned_len) > mem_size() - 0x100000) {
            fprintf(stderr, "mmap2: out of arena space (ptr=0x%08X len=0x%X)\n", g_mmap_ptr, aligned_len);
            return (uint32_t)-ENOMEM;
        }

        result_addr = g_mmap_ptr;
        g_mmap_ptr += aligned_len;
    }

    // zero out the region like a real kernel would
    // the memory is already calloc'd at startup but callers expect it to be zero
    uint8_t *host = guest_to_host(result_addr);
    if (!host) {
        fprintf(stderr, "mmap2: guest_to_host failed for addr=0x%08X\n", result_addr);
        return (uint32_t)-EFAULT;
    }
    memset(host, 0, aligned_len);
    printf("mmap2: addr=0x%08X len=0x%08X flags=0x%X fd=%d -> 0x%08X\n",
               addr, len, flags, fd, result_addr);
    return result_addr;
}

int32_t sys_fstat64(i386 *cpu, int fd, uint32_t buf_addr) {
    (void)cpu;
    if (fd < 0 || fd >= MAX_FDS || !fd_table[fd].used) return -EBADF;

    LinuxStat64 *st = guest_to_host(buf_addr);
    if (!st) return -EFAULT;

    memset(st, 0, sizeof(*st));

#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(fd_table[fd].handle, &info)) {
        // stdin/stdout/stderr return 0 for everything, glibc doesn't care
        if (fd <= 2) return 0;
        return -5; // EIO
    }
    // fake a regular file, glibc mostly just looks at st_mode and st_size
    st->st_mode   = 0100644; // S_IFREG | 0644
    st->st_nlink  = 1;
    st->st_size   = ((int64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    st->st_blksize = 4096;
    st->st_blocks  = (st->st_size + 511) / 512;
    // for stdin/stdout set S_IFCHR so glibc knows it's a char device
    if (GetFileType(fd_table[fd].handle) == FILE_TYPE_CHAR)
        st->st_mode = 0020620; // S_IFCHR | 0620
#else
    struct stat host_st;
    if (fstat(fd_table[fd].host_fd, &host_st) < 0) return -(int32_t)errno;
    st->st_dev    = (uint64_t)host_st.st_dev;
    st->st_ino    = (uint64_t)host_st.st_ino;
    st->st_mode   = (uint32_t)host_st.st_mode;
    st->st_nlink  = (uint32_t)host_st.st_nlink;
    st->st_uid    = (uint32_t)host_st.st_uid;
    st->st_gid    = (uint32_t)host_st.st_gid;
    st->st_rdev   = (uint64_t)host_st.st_rdev;
    st->st_size   = (int64_t)host_st.st_size;
    st->st_blksize = (uint32_t)host_st.st_blksize;
    st->st_blocks  = (uint64_t)host_st.st_blocks;
    st->st_atime  = (uint32_t)host_st.st_atime;
    st->st_mtime  = (uint32_t)host_st.st_mtime;
    st->st_ctime  = (uint32_t)host_st.st_ctime;
#endif

    return 0;
}

// we don't actually deliver signals so these just need to succeed
// glibc installs handlers for SIGFPE, SIGSEGV etc during startup,
// we just pretend to register them

#define SIGMAX 64

typedef struct {
    uint32_t handler;
    uint32_t sa_flags;
    uint32_t sa_restorer;
    uint8_t  sa_mask[8]; // 64-bit mask
} LinuxSigaction;

static LinuxSigaction g_sigactions[SIGMAX];

int32_t sys_rt_sigaction(i386 *cpu, int sig, uint32_t act_addr, uint32_t oact_addr, uint32_t sigsetsize) {
    (void)cpu; (void)sigsetsize;
    if (sig < 1 || sig >= SIGMAX) return -EINVAL;

    if (oact_addr) {
        LinuxSigaction *oact = guest_to_host(oact_addr);
        if (!oact) return -EFAULT;
        memcpy(oact, &g_sigactions[sig], sizeof(LinuxSigaction));
    }

    if (act_addr) {
        LinuxSigaction *act = guest_to_host(act_addr);
        if (!act) return -EFAULT;
        memcpy(&g_sigactions[sig], act, sizeof(LinuxSigaction));
    }

    return 0;
}

int32_t sys_rt_sigprocmask(i386 *cpu, int how, uint32_t set_addr, uint32_t oset_addr, uint32_t sigsetsize) {
    (void)cpu; (void)how; (void)set_addr; (void)sigsetsize;
    // single threaded and no signal delivery, just return the empty set
    if (oset_addr) {
        void *oset = guest_to_host(oset_addr);
        if (!oset) return -EFAULT;
        memset(oset, 0, sigsetsize < 8 ? sigsetsize : 8);
    }
    return 0;
}

int32_t sys_set_tid_address(i386 *cpu, uint32_t tidptr) {
    (void)cpu;
    // write our fake TID into the guest's clear_child_tid pointer
    // glibc uses this for pthreads exit notification, just return the tid
    if (tidptr) {
        uint32_t *p = guest_to_host(tidptr);
        if (p) *p = 1; // tid 1
    }
    return 1; // return our fake tid
}

uint32_t g_brk = 0;

void init_brk(uint32_t base) {
    g_brk = base;
    printf("brk: heap base at 0x%08X\n", g_brk);
}

uint32_t sys_brk(i386 *cpu, uint32_t new_brk) {
    (void)cpu;
    if (new_brk == 0) return g_brk;
    if (new_brk < g_brk) return g_brk;
    if ((size_t)new_brk >= mem_size()) {
        fprintf(stderr, "sys_brk: 0x%08X exceeds guest memory\n", new_brk);
        return g_brk;
    }
    uint32_t old_brk = g_brk;
    uint32_t alloc_size = new_brk - old_brk;
    // glibc calls realloc() on memory it allocated via sbrk/brk
    // without a valid chunk header, malloc_printerr fires immediately
    if (alloc_size >= 8) {
        mem_write32(old_brk + 0, 0);
        mem_write32(old_brk + 4, alloc_size | 1); // size | PREV_INUSE
    }
    g_brk = new_brk;
    return g_brk;
}

int32_t sys_writev(i386 *cpu, int fd, uint32_t iov_addr, int iovcnt) {
    (void)cpu;
    int32_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        LinuxIovec *iov = guest_to_host(iov_addr + (uint32_t)i * 8);
        if (!iov) return -EFAULT;
        if (iov->iov_len == 0) continue;
        int32_t n = sys_write(cpu, fd, iov->iov_base, iov->iov_len);
        if (n < 0) return n;
        total += n;
    }
    return total;
}

int32_t sys_set_thread_area(i386 *cpu, uint32_t u_info_addr) {
    (void)cpu;
    UserDesc *desc = guest_to_host(u_info_addr);
    if (!desc) return -EFAULT;
    if (desc->entry_number == (uint32_t)-1) desc->entry_number = 0;
    set_gs_base(desc->base_addr);

    // glibc already set up TCB (dtv, self-ptr, etc.) before calling us.
    // don't touch any of that. only set the vsyscall stub slot.
    mem_write32(desc->base_addr + 0x10, 0x00001000);

    // write a valid malloc chunk header at [base_addr - 8] so that
    // _dl_allocate_tls_init can call realloc() on the DTV without aborting.
    // size must be >= request2size(0x88) = 0x90, so use 0x91 (0x90 | PREV_INUSE).
    mem_write32(desc->base_addr - 8, 0);     // prev_size
    mem_write32(desc->base_addr - 4, 0x91);  // size=0x90 | PREV_INUSE

    printf("set_thread_area: GS base set to 0x%08X\n", desc->base_addr);
    return 0;
}
