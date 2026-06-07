#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu/cpu.h"
#include "mem/mem.h"
#include "loader/loader.h"
#include "syscall/syscall.h"

#define STUB_ADDR     0x00001000
#define FAKE_TLS_SIZE 0x400

extern void init_opcode_table(void);
extern void init_mmap_arena(uint32_t mmap_base);

int main(int argc, char **argv) {
    const char *progname = strrchr(argv[0], '\\');
    if (argc < 2) {
        if (progname)
            progname++;
        else
            progname = argv[0];
        fprintf(stderr, "usage: ./%s <elf binary>\n", progname);
        return EXIT_FAILURE;
    }

    init_mem(1024 * 1024 * 512);

    init_opcode_table();
    init_fd_table();

    i386 cpu;
    init_cpu(&cpu);

    // int 0x80; ret, __kernel_vsyscall trampoline, glibc calls syscalls through GS:0x10
    // which points here, so this needs to dispatch via int 0x80 not just return
    mem_write8(STUB_ADDR + 0, 0xCD);
    mem_write8(STUB_ADDR + 1, 0x80);
    mem_write8(STUB_ADDR + 2, 0xC3);

    LoadInfo load_info;
    if (!load_elf(argv[1], &cpu, &load_info, argc, argv)) {
        fprintf(stderr, "failed to load %s\n", argv[1]);
        free_mem();
        return EXIT_FAILURE;
    }

    init_brk(load_info.load_end);

    // fake TLS block sits right above the loaded binary, before the heap
    uint32_t chunk_size = (FAKE_TLS_SIZE + 8 + 7) & ~7u;
    uint32_t chunk_base = load_info.load_end;
    uint32_t fake_tls_base = chunk_base + 8;

    mem_write32(chunk_base + 0, 0);
    mem_write32(chunk_base + 4, chunk_size | 1);

    extern uint32_t g_brk;
    g_brk = chunk_base + chunk_size;

    for (uint32_t i = 0; i < FAKE_TLS_SIZE; i += 4)
        mem_write32(fake_tls_base + i, STUB_ADDR);

    for (uint32_t i = 4; i <= 0x400; i += 4)
        mem_write32(fake_tls_base - i, 0);

    mem_write32(fake_tls_base + 0x00, STUB_ADDR);
    mem_write32(fake_tls_base + 0x04, fake_tls_base);
    mem_write32(fake_tls_base + 0x08, fake_tls_base);
    mem_write32(fake_tls_base + 0x14, 0xDEADBEEF);

    set_gs_base(fake_tls_base);

    // mmap arena starts at 0x10000000 and grows up, giving us 128MB before the stack
    // adjust this if the loaded binary happens to land there
    uint32_t mmap_arena_start = 0x0C000000;
    if (load_info.load_end > mmap_arena_start)
        mmap_arena_start = (load_info.load_end + 0x00FFFFFF) & ~0x00FFFFFFu;
    init_mmap_arena(mmap_arena_start);

    printf("loader: fake TLS at 0x%08X (chunk at 0x%08X)\n", fake_tls_base, chunk_base);
    printf("starting emulation at EIP: 0x%08X\n", cpu.eip);

    /*
    printf("stack dump:\n");
    for (uint32_t a = cpu.regs[REG_ESP]; a < cpu.regs[REG_ESP] + 0x80; a += 4)
        printf("  [0x%08X] = 0x%08X\n", a, mem_read32(a));
    */

    g_trace = 0;
    emulate(&cpu);

    printf("halted. EIP: 0x%08X  cycles: %d\n", cpu.eip, cpu.cycles);

    free_mem();
    return EXIT_SUCCESS;
}
