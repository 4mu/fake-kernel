#ifndef LOADER_H
#define LOADER_H

#include "../cpu/cpu.h"

typedef struct {
    uint32_t load_end; // highest address used by PT_LOAD segments
} LoadInfo;

// parses an ELF32 binary, maps its PT_LOAD segments into guest memory,
// sets cpu->eip to the entry point and builds a basic argv stack
// returns 1 on success, 0 on failure
int load_elf(const char *path, i386 *cpu, LoadInfo *info, int argc, char **argv);

#endif
