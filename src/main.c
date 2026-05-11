#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// these paths assume gcc is invoked from the src/ directory
// e.g. gcc cpu/cpu.c cpu/ops.c mem/mem.c ... main.c
#include "cpu/cpu.h"
#include "mem/mem.h"
#include "loader/loader.h"
#include "syscall/syscall.h"

#define FAKE_TLS_BASE 0x00010000
#define FAKE_TLS_SIZE 256

// lives in ops.c, sets up the 256 entry function pointer table
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

    // 64mb for the guest, probably enough for now
    init_mem(1024 * 1024 * 256);

    init_opcode_table();
    init_fd_table();

    i386 cpu;
    init_cpu(&cpu);

    for (int i = 0; i < FAKE_TLS_SIZE; i++) mem_write8(FAKE_TLS_BASE + i, 0);
    set_gs_base(FAKE_TLS_BASE);

    LoadInfo load_info;

    if (!load_elf(argv[1], &cpu, &load_info)) {
        fprintf(stderr, "failed to load %s\n", argv[1]);
        free_mem();
        return EXIT_FAILURE;
    }

    init_brk(load_info.load_end);

    printf("starting emulation at EIP: 0x%08X\n", cpu.eip);

    emulate(&cpu);

    printf("halted. EIP: 0x%08X  cycles: %d\n", cpu.eip, cpu.cycles);

    free_mem();
    return EXIT_SUCCESS;
}
