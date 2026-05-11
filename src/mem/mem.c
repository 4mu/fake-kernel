#include "mem.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static uint8_t *g_mem = NULL;
static size_t g_size = 0;

void init_mem(size_t size) {
    g_mem = (uint8_t *)calloc(1, size);
    if (!g_mem) {
        fprintf(stderr, "mem: failed to allocate %zu bytes\n", size);
        exit(EXIT_FAILURE);
    }
    g_size = size;
}

void free_mem(void) {
    free(g_mem);
    g_mem = NULL;
    g_size = 0;
}

size_t mem_size(void) { return g_size;}

static inline int check_bounds(uint32_t addr, size_t width, const char *op) {
    if ((size_t)addr + width > g_size) {
        fprintf(stderr, "mem: %s out of bounds at 0x%08X (guest size 0x%zX)\n", op, addr, g_size);
        return 0;
    }
    return 1;
}

void *guest_to_host(uint32_t addr) {
    if (!check_bounds(addr, 1, "guest_to_host")) return NULL;
    return g_mem + addr;
}

// reads are all manual byte by byte so we don't hit alignment UB

uint8_t mem_read8(uint32_t addr) {
    if (!check_bounds(addr, 1, "read8")) return 0;
    return g_mem[addr];
}

uint16_t mem_read16(uint32_t addr) {
    if (!check_bounds(addr, 2, "read16")) return 0;
    return (uint16_t)g_mem[addr]| (uint16_t)(g_mem[addr + 1] << 8);
}

uint32_t mem_read32(uint32_t addr) {
    if (!check_bounds(addr, 4, "read32")) return 0;
    return (uint32_t)g_mem[addr]
         | (uint32_t)(g_mem[addr + 1] <<  8)
         | (uint32_t)(g_mem[addr + 2] << 16)
         | (uint32_t)(g_mem[addr + 3] << 24);
}

void mem_write8(uint32_t addr, uint8_t val) {
    if (!check_bounds(addr, 1, "write8")) return;
    g_mem[addr] = val;
}

void mem_write16(uint32_t addr, uint16_t val) {
    if (!check_bounds(addr, 2, "write16")) return;
    g_mem[addr] = (uint8_t)(val);
    g_mem[addr + 1] = (uint8_t)(val >> 8);
}

void mem_write32(uint32_t addr, uint32_t val) {
    if (!check_bounds(addr, 4, "write32")) return;
    g_mem[addr] = (uint8_t)(val);
    g_mem[addr + 1] = (uint8_t)(val >>  8);
    g_mem[addr + 2] = (uint8_t)(val >> 16);
    g_mem[addr + 3] = (uint8_t)(val >> 24);
}

void mem_copy_in(uint32_t guest_dst, const void *host_src, size_t len) {
    if (!check_bounds(guest_dst, len, "mem_copy_in")) return;
    memcpy(g_mem + guest_dst, host_src, len);
}
