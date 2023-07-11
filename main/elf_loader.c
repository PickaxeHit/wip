//
// Created by pickaxehit on 2023/7/11.
//

#include "elf_loader.h"
#include "elf.h"
#include <stddef.h>
#include <sys/types.h>

typedef struct elf_loader_section_s {
    void *data;
    int secidx;
    size_t size;
    off_t relsecidx;
    struct elf_loader_section_s *next;
} elf_loader_section_t;


struct elf_loader_ctx_s {
    LOADER_FD_T fd;
    void *exec;
    void *text;
    const elf_loader_env_t *env;

    size_t e_shnum;
    off_t e_shoff;
    off_t shstrtab_offset;

    size_t symtab_count;
    off_t symtab_offset;
    off_t strtab_offset;

    elf_loader_section_t *section;
};
