#ifndef MEM_H
#define MEM_H

#include <stdint.h>
#include <stddef.h>

// call once at startup, allocates the flat guest address space
void init_mem(size_t size);

extern uint32_t g_watch_addr;

void free_mem(void);

extern uint32_t g_last_eip;

// translates a guest address to a real host pointer
// returns NULL if the address is out of range
void *guest_to_host(uint32_t addr);

// all reads and writes are done byte by byte to avoid alignment UB
uint8_t mem_read8(uint32_t addr);
uint16_t mem_read16(uint32_t addr);
uint32_t mem_read32(uint32_t addr);

void mem_write8(uint32_t addr, uint8_t  val);
void mem_write16(uint32_t addr, uint16_t val);
void mem_write32(uint32_t addr, uint32_t val);

// bulk copy from host into guest memory, used by the loader
void mem_copy_in(uint32_t guest_dst, const void *host_src, size_t len);

// loader needs this to know where to put the stack
size_t mem_size(void);

#endif
