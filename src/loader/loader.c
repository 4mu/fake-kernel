#include "loader.h"
#include "../mem/mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define ELF_MAGIC    "\x7f" "ELF"
#define ET_EXEC       2
#define EM_386        3
#define PT_LOAD       1
#define ELFCLASS32    1
#define ELFDATA2LSB   1

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} Elf32_Phdr;

// write a 32-bit value at addr and bump addr forward (for forward-building the stack block)
static void put32(uint32_t *addr, uint32_t val) {
    mem_write32(*addr, val);
    *addr += 4;
}

int load_elf(const char *path, i386 *cpu, LoadInfo *info, int argc, char **argv) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror("loader: fopen");
        return 0;
    }

    Elf32_Ehdr ehdr;
    if (fread(&ehdr, sizeof(ehdr), 1, f) != 1) {
        fprintf(stderr, "loader: couldn't read ELF header\n");
        fclose(f);
        return 0;
    }

    if (memcmp(ehdr.e_ident, ELF_MAGIC, 4) != 0) {
        fprintf(stderr, "loader: not an ELF file\n");
        fclose(f);
        return 0;
    }
    if (ehdr.e_ident[4] != ELFCLASS32) {
        fprintf(stderr, "loader: not a 32 bit ELF (class=%d)\n", ehdr.e_ident[4]);
        fclose(f); return 0;
    }
    if (ehdr.e_ident[5] != ELFDATA2LSB) {
        fprintf(stderr, "loader: not little endian\n");
        fclose(f); return 0;
    }
    if (ehdr.e_type != ET_EXEC) {
        fprintf(stderr, "loader: not an executable (type=%d)\n", ehdr.e_type);
        fclose(f); return 0;
    }
    if (ehdr.e_machine != EM_386) {
        fprintf(stderr, "loader: not i386 (machine=%d)\n", ehdr.e_machine);
        fclose(f); return 0;
    }
    if (ehdr.e_phentsize < sizeof(Elf32_Phdr)) {
        fprintf(stderr, "loader: phentsize too small (%d)\n", ehdr.e_phentsize);
        fclose(f); return 0;
    }

    uint32_t load_end   = 0;
    uint32_t load_addr  = 0;  // vaddr of the first PT_LOAD, used for AT_PHDR
    int      first_load = 1;

    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (fseek(f, (long)(ehdr.e_phoff + (uint32_t)i * ehdr.e_phentsize), SEEK_SET) != 0) {
            fprintf(stderr, "loader: fseek to phdr %d failed\n", i);
            fclose(f); return 0;
        }
        Elf32_Phdr phdr;
        if (fread(&phdr, sizeof(phdr), 1, f) != 1) {
            fprintf(stderr, "loader: couldn't read phdr %d\n", i);
            fclose(f); return 0;
        }
        if (phdr.p_type != PT_LOAD) continue;
        if (phdr.p_memsz == 0) continue;
        if ((size_t)phdr.p_vaddr + phdr.p_memsz > mem_size()) {
            fprintf(stderr, "loader: segment %d at 0x%08X is too big for guest memory\n", i, phdr.p_vaddr);
            fclose(f); return 0;
        }

        if (first_load) {
            load_addr = phdr.p_vaddr;
            first_load = 0;
        }

        // zero the full memsz (handles BSS)
        uint8_t *host = (uint8_t *)mem_size();  // just to get a non-null check
        (void)host;
        for (uint32_t b = 0; b < phdr.p_memsz; b++) mem_write8(phdr.p_vaddr + b, 0);

        if (phdr.p_filesz > 0) {
            if (fseek(f, (long)phdr.p_offset, SEEK_SET) != 0) {
                fprintf(stderr, "loader: fseek to segment %d data failed\n", i);
                fclose(f); return 0;
            }
            uint32_t remaining = phdr.p_filesz;
            uint32_t dst = phdr.p_vaddr;
            uint8_t chunk[4096];
            while (remaining > 0) {
                uint32_t to_read = remaining < sizeof(chunk) ? remaining : (uint32_t)sizeof(chunk);
                if (fread(chunk, 1, to_read, f) != to_read) {
                    fprintf(stderr, "loader: short read on segment %d\n", i);
                    fclose(f); return 0;
                }
                mem_copy_in(dst, chunk, to_read);
                dst += to_read;
                remaining -= to_read;
            }
        }

        uint32_t seg_end = phdr.p_vaddr + phdr.p_memsz;
        if (seg_end > load_end) load_end = seg_end;

        printf("loader: segment %d  vaddr=0x%08X  filesz=0x%X  memsz=0x%X\n",
               i, phdr.p_vaddr, phdr.p_filesz, phdr.p_memsz);
    }

    info->load_end = (load_end + 0xFFF) & ~0xFFFu;
    cpu->eip = ehdr.e_entry;
    printf("loader: entry point = 0x%08X\n", ehdr.e_entry);
    fclose(f);

    // AT_PHDR is the virtual address of the program header table in the loaded image.
    // For a non-PIE ET_EXEC binary the phdr table is at load_addr + e_phoff.
    uint32_t at_phdr = load_addr + ehdr.e_phoff;

    // ---- build the initial stack ----
    //
    // The Linux ABI (and QEMU) lay things out like this in memory, low to high:
    //
    //   [argc]          <- ESP points here on entry to _start
    //   [argv[0]]
    //   ...
    //   [argv[n-1]]
    //   [NULL]          <- end of argv
    //   [NULL]          <- end of envp (we pass no env vars)
    //   [auxv[0].type]
    //   [auxv[0].val]
    //   ...
    //   [AT_NULL, 0]    <- end of auxv
    //   <padding to align the string area>
    //   [argv strings]
    //   [16 random bytes for AT_RANDOM]
    //
    // QEMU pre-computes the block size, aligns the bottom, then writes forward.
    // We do the same.

    // first pass: figure out how much space we need for the strings
    int guest_argc = argc - 1;
    if (guest_argc > 63) guest_argc = 63;

    // count aux entries: AT_PHDR AT_PHENT AT_PHNUM AT_RANDOM AT_ENTRY AT_PAGESZ
    //                    AT_HWCAP AT_CLKTCK AT_UID AT_EUID AT_GID AT_EGID AT_SECURE
    //                    AT_BASE AT_FLAGS AT_NULL = 16 entries = 32 words
    #define N_AUXV_ENTRIES 16

    // words: 1 (argc) + (argc+1) (argv ptrs + NULL) + 1 (envp NULL) + 32 (auxv)
    uint32_t header_words = 1 + (uint32_t)(guest_argc + 1) + 1 + (N_AUXV_ENTRIES * 2);
    uint32_t header_bytes = header_words * 4;

    // string space: argv strings + 16 random bytes, place at top of guest mem
    uint32_t str_area_top = (uint32_t)mem_size() - 4;

    // figure out string sizes
    uint32_t string_bytes = 16; // for AT_RANDOM
    for (int i = 0; i < guest_argc; i++)
        string_bytes += (uint32_t)(strlen(argv[i + 1]) + 1);

    // the bottom of the string area (where the header block ends)
    // align to 16 so the whole block starts 16-byte aligned
    uint32_t str_area_base = str_area_top - string_bytes;
    str_area_base &= ~0xFu;  // align down to 16

    // the stack pointer starts at str_area_base - header_bytes, then aligned
    uint32_t esp_base = (str_area_base - header_bytes) & ~0xFu;

    // write all the strings into the string area
    uint32_t sptr = str_area_base;
    uint32_t argv_addrs[64];
    for (int i = 0; i < guest_argc; i++) {
        size_t len = strlen(argv[i + 1]) + 1;
        argv_addrs[i] = sptr;
        mem_copy_in(sptr, argv[i + 1], len);
        sptr += (uint32_t)len;
    }

    // 16 random bytes for AT_RANDOM — place them right after the argv strings
    uint32_t random_addr = sptr;
    mem_write32(sptr + 0,  0xDEADBEEF);
    mem_write32(sptr + 4,  0xCAFEBABE);
    mem_write32(sptr + 8,  0x13371337);
    mem_write32(sptr + 12, 0xABCDEF01);

    // now write the header block forward from esp_base
    uint32_t w = esp_base;

    // argc
    put32(&w, (uint32_t)guest_argc);

    // argv pointers
    for (int i = 0; i < guest_argc; i++)
        put32(&w, argv_addrs[i]);
    put32(&w, 0); // argv NULL terminator

    // envp NULL terminator (no env vars)
    put32(&w, 0);

    // auxv — written as (type, value) pairs
    // order matches what Linux kernel puts out (see fs/binfmt_elf.c)
    put32(&w, 3);  put32(&w, at_phdr);              // AT_PHDR
    put32(&w, 4);  put32(&w, ehdr.e_phentsize);      // AT_PHENT
    put32(&w, 5);  put32(&w, ehdr.e_phnum);          // AT_PHNUM
    put32(&w, 6);  put32(&w, 0x1000);                // AT_PAGESZ
    put32(&w, 7);  put32(&w, 0);                     // AT_BASE (no interp)
    put32(&w, 8);  put32(&w, 0);                     // AT_FLAGS
    put32(&w, 9);  put32(&w, ehdr.e_entry);          // AT_ENTRY
    put32(&w, 11); put32(&w, 1000);                  // AT_UID
    put32(&w, 12); put32(&w, 1000);                  // AT_EUID
    put32(&w, 13); put32(&w, 1000);                  // AT_GID
    put32(&w, 14); put32(&w, 1000);                  // AT_EGID
    put32(&w, 16); put32(&w, 0x00008910);            // AT_HWCAP (CMOV|CX8|TSC|MMX)
    put32(&w, 17); put32(&w, 100);                   // AT_CLKTCK
    put32(&w, 23); put32(&w, 0);                     // AT_SECURE
    put32(&w, 25); put32(&w, random_addr);           // AT_RANDOM
    put32(&w, 0);  put32(&w, 0);                     // AT_NULL

    // the ABI says ESP must be 16-byte aligned on _start entry
    // _start does its own alignment, we just need (esp + 4) % 16 == 0
    // so esp % 16 == 12 on entry
    // esp_base = (esp_base & ~0xFu) - 4;

    cpu->regs[REG_ESP] = esp_base;

    printf("loader: AT_PHDR=0x%08X  AT_RANDOM=0x%08X\n", at_phdr, random_addr);
    printf("loader: stack at 0x%08X  argc=%d\n", esp_base, guest_argc);

    return 1;
    #undef N_AUXV_ENTRIES
}
