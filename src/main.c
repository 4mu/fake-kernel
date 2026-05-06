#include <stdio.h>
#include <stdlib.h>

#include "src/cpu/cpu.h"
#include "src/mem/mem.h"
#include "src/loader/loader.h"

// Defined in ops.c - ensures the function pointer table is populated
extern void init_opcode_table(void);

int main(int argc, char **argv) {
    // 1. Basic Argument Check
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <path_to_linux_elf>\n", argv[0]);
        return EXIT_FAILURE;
    }



    // 2. Initialize Subsystems
    // Allocate 64MB for the guest address space (adjustable)
    init_mem(1024 * 1024 * 64);

    // Populate the 256-entry opcode function pointer table
    init_opcode_table();

    i386 cpu;
    init_cpu(&cpu);

    // 3. Load the Linux ELF Binary
    // The loader parses the ELF header, maps segments into guest memory,
    // and sets the CPU's EIP to the entry point and ESP to the initial stack.
    if (!load_elf(argv[1], &cpu)) {
        fprintf(stderr, "Error: Failed to load or parse ELF binary: %s\n", argv[1]);
        free_mem();
        return EXIT_FAILURE;
    }

    // 4. Start Emulation
    printf("Starting emulation at EIP: 0x%08X\n", cpu.eip);

    // The emulate loop runs until cpu.halted is set (e.g., by SYS_EXIT)
    emulate(&cpu);

    // 5. Cleanup and Exit Status
    printf("\n--------------------------------------\n");
    printf("Emulation Halted.\n");
    printf("Final EIP: 0x%08X\n", cpu.eip);
    printf("Total Cycles: %d\n", cpu.cycles);

    free_mem();
    return EXIT_SUCCESS;
}
