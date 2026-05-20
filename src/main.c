#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpu/cpu.h"
#include "mem/mem.h"
#include "loader/loader.h"
#include "syscall/syscall.h"

#define STUB_ADDR        0x00001000
#define FAKE_TLS_SIZE    0x400

extern void init_opcode_table(void);

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

    init_mem(1024 * 1024 * 256);

    init_opcode_table();
    init_fd_table();

    i386 cpu;
    init_cpu(&cpu);

    mem_write8(STUB_ADDR + 0, 0x31);
    mem_write8(STUB_ADDR + 1, 0xC0);
    mem_write8(STUB_ADDR + 2, 0xC3);

    LoadInfo load_info;
    if (!load_elf(argv[1], &cpu, &load_info, argc, argv)) {
        fprintf(stderr, "failed to load %s\n", argv[1]);
        free_mem();
        return EXIT_FAILURE;
    }

    init_brk(load_info.load_end);

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

    // GS:[0] is called as a function pointer by glibc, keep it as the stub
    mem_write32(fake_tls_base + 0x00, STUB_ADDR);

    // self pointer at both offsets glibc might look at
    mem_write32(fake_tls_base + 0x04, fake_tls_base);
    mem_write32(fake_tls_base + 0x08, fake_tls_base);

    mem_write32(fake_tls_base + 0x14, 0xDEADBEEF);

    set_gs_base(fake_tls_base);

    printf("loader: fake TLS at 0x%08X (chunk at 0x%08X)\n", fake_tls_base, chunk_base);

    printf("starting emulation at EIP: 0x%08X\n", cpu.eip);
    emulate(&cpu);

    printf("halted. EIP: 0x%08X  cycles: %d\n", cpu.eip, cpu.cycles);

    free_mem();
    return EXIT_SUCCESS;
}
