#include "loader.h"
#include "../mem/mem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// rolling our own ELF structs so we don't need elf.h which isn't on Windows
#define ELF_MAGIC     "\x7f" "ELF"
#define ET_EXEC       2
#define EM_386        3
#define PT_LOAD       1
#define ELFCLASS32    1
#define ELFDATA2LSB   1  // little endian

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;    // offset to program header table
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
    uint32_t p_offset;   // where in the file this segment starts
    uint32_t p_vaddr;    // where in guest memory to put it
    uint32_t p_paddr;
    uint32_t p_filesz;   // how many bytes to copy from the file
    uint32_t p_memsz;    // how many bytes to reserve (memsz >= filesz, extra is BSS)
    uint32_t p_flags;
    uint32_t p_align;
} Elf32_Phdr;

// initial stack layout (grows down, high addr at top):
//
//   strings for argv[0] etc
//   NULL          end of envp (empty, we don't pass any env)
//   NULL          end of argv
//   argv[0] ptr
//   argc          ESP ends up pointing here

static void stack_push32(uint32_t *esp, uint32_t val) {
    *esp -= 4;
    mem_write32(*esp, val);
}

// writes a string into guest memory at str_ptr and bumps str_ptr forward
// returns the guest address of the string
static uint32_t push_string(uint32_t *str_ptr, const char *s) {
    size_t len = strlen(s) + 1;
    uint32_t addr = *str_ptr;
    mem_copy_in(addr, s, len);
    *str_ptr += (uint32_t)len;
    return addr;
}

int load_elf(const char *path, i386 *cpu, LoadInfo *info, int argc, char **argv) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror("loader: fopen");
        return 0;
    }

    // validate the ELF header

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
        fclose(f);
        return 0;
    }

    if (ehdr.e_ident[5] != ELFDATA2LSB) {
        fprintf(stderr, "loader: not little endian\n");
        fclose(f);
        return 0;
    }

    if (ehdr.e_type != ET_EXEC) {
        fprintf(stderr, "loader: not an executable (type=%d)\n", ehdr.e_type);
        fclose(f);
        return 0;
    }

    if (ehdr.e_machine != EM_386) {
        fprintf(stderr, "loader: not i386 (machine=%d)\n", ehdr.e_machine);
        fclose(f);
        return 0;
    }

    if (ehdr.e_phentsize < sizeof(Elf32_Phdr)) {
        fprintf(stderr, "loader: phentsize too small (%d)\n", ehdr.e_phentsize);
        fclose(f);
        return 0;
    }

    uint32_t load_end = 0;

    // walk the program headers and load any PT_LOAD segments into guest memory

    for (int i = 0; i < ehdr.e_phnum; i++) {
        long off = (long)(ehdr.e_phoff + (uint32_t)i * ehdr.e_phentsize);

        if (fseek(f, off, SEEK_SET) != 0) {
            fprintf(stderr, "loader: fseek to phdr %d failed\n", i);
            fclose(f);
            return 0;
        }

        Elf32_Phdr phdr;

        if (fread(&phdr, sizeof(phdr), 1, f) != 1) {
            fprintf(stderr, "loader: couldn't read phdr %d\n", i);
            fclose(f);
            return 0;
        }

        if (phdr.p_type != PT_LOAD) continue;

        if (phdr.p_memsz == 0) continue;

        if ((size_t)phdr.p_vaddr + phdr.p_memsz > mem_size()) {
            fprintf(stderr, "loader: segment %d at 0x%08X is too big for guest memory\n", i, phdr.p_vaddr);
            fclose(f);
            return 0;
        }

        // zero the whole region first so BSS is handled automatically

        uint32_t vaddr = phdr.p_vaddr;

        for (uint32_t b = 0; b < phdr.p_memsz; b++) mem_write8(vaddr + b, 0);

        if (phdr.p_filesz > 0) {
            if (fseek(f, (long)phdr.p_offset, SEEK_SET) != 0) {
                fprintf(stderr, "loader: fseek to segment %d data failed\n", i);
                fclose(f);
                return 0;
            }

            // read in 4k chunks so we're not mallocing the whole segment on the host

            uint32_t remaining = phdr.p_filesz;
            uint32_t dst = vaddr;
            uint8_t chunk[4096];

            while (remaining > 0) {
                uint32_t to_read = remaining < sizeof(chunk) ? remaining : (uint32_t)sizeof(chunk);

                if (fread(chunk, 1, to_read, f) != to_read) {
                    fprintf(stderr, "loader: short read on segment %d\n", i);
                    fclose(f);
                    return 0;
                }

                mem_copy_in(dst, chunk, to_read);

                dst += to_read;
                remaining -= to_read;
            }
        }

        uint32_t seg_end = phdr.p_vaddr + phdr.p_memsz;

        if (seg_end > load_end) load_end = seg_end;

        printf("loader: segment %d  vaddr=0x%08X  filesz=0x%X  memsz=0x%X\n", i, phdr.p_vaddr, phdr.p_filesz, phdr.p_memsz);
    }

    info->load_end = (load_end + 0xFFF) & ~0xFFFu;

    cpu->eip = ehdr.e_entry;

    printf("loader: entry point = 0x%08X\n", ehdr.e_entry);

    fclose(f);

    // put strings at the very top, stack grows down below them
    uint32_t str_ptr = (uint32_t)mem_size() - 4096;
    uint32_t esp = str_ptr - 4;

    uint32_t argv_addrs[64];
    int guest_argc = argc - 1;
    if (guest_argc > 63) guest_argc = 63;

    for (int i = 0; i < guest_argc; i++) argv_addrs[i] = push_string(&str_ptr, argv[i + 1]);

    // push AT_NULL first so it ends up at the TOP (highest address, read last)
    stack_push32(&esp, 0);
    stack_push32(&esp, 0);

    // then the rest in any order, they end up below AT_NULL
    stack_push32(&esp, 64);
    stack_push32(&esp, 28);    // AT_L1D_CACHE_LINESIZE

    stack_push32(&esp, 64);
    stack_push32(&esp, 27);    // AT_L1I_CACHE_LINESIZE

    stack_push32(&esp, 0);
    stack_push32(&esp, 8);     // AT_FLAGS

    stack_push32(&esp, 0);
    stack_push32(&esp, 7);     // AT_BASE

    stack_push32(&esp, 0);
    stack_push32(&esp, 23);    // AT_SECURE

    stack_push32(&esp, 1000);
    stack_push32(&esp, 14);    // AT_EGID

    stack_push32(&esp, 1000);
    stack_push32(&esp, 13);    // AT_GID

    stack_push32(&esp, 1000);
    stack_push32(&esp, 12);    // AT_EUID

    stack_push32(&esp, 1000);
    stack_push32(&esp, 11);    // AT_UID

    stack_push32(&esp, 100);
    stack_push32(&esp, 17);    // AT_CLKTCK

    stack_push32(&esp, 0x1000);
    stack_push32(&esp, 6);     // AT_PAGESZ

    stack_push32(&esp, ehdr.e_entry);
    stack_push32(&esp, 9);     // AT_ENTRY

    stack_push32(&esp, ehdr.e_phnum);
    stack_push32(&esp, 5);     // AT_PHNUM

    stack_push32(&esp, ehdr.e_phentsize);
    stack_push32(&esp, 4);     // AT_PHENT

    // push AT_PHDR last so it ends up at the BOTTOM (lowest address, read first)
    stack_push32(&esp, ehdr.e_phoff + 0x08048000);
    stack_push32(&esp, 3);     // AT_PHDR

    // end of envp
    stack_push32(&esp, 0);

    // end of argv
    stack_push32(&esp, 0);

    // argv pointers, last to first
    for (int i = guest_argc - 1; i >= 0; i--) stack_push32(&esp, argv_addrs[i]);

    // argc
    stack_push32(&esp, (uint32_t)guest_argc);

    cpu->regs[REG_ESP] = esp;

    /*
    printf("loader: stack dump:\n");
    for (uint32_t a = esp; a < esp + 0x40; a += 4)
        printf("  [0x%08X] = 0x%08X\n", a, mem_read32(a));
    */

    printf("loader: stack at 0x%08X  argc=%d\n", esp, guest_argc);

    uint32_t at_phdr = ehdr.e_phoff + 0x08048000;
    printf("loader: AT_PHDR=0x%08X\n", at_phdr);
    for (int i = 0; i < ehdr.e_phnum; i++) {
        uint32_t entry = at_phdr + i * ehdr.e_phentsize;
        uint32_t type = mem_read32(entry);
        if (type == 7) {
            uint32_t align = mem_read32(entry + 28);
            printf("loader: PT_TLS phdr[%d] in guest memory: p_align=0x%X\n", i, align);
        }
    }

    return 1;
}
