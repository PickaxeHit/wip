//
// Created by pickaxehit on 2023/7/11.
//

#ifndef MAIN_ELF_LOADER_H
#define MAIN_ELF_LOADER_H

#include <stdint.h>

#define LOADER_FD_T void *

typedef struct elf_loader_symbol_s {
    const char *name; /*!< Name of symbol */
    void *ptr;        /*!< Pointer of symbol in memory */
} elf_loader_symbol_t;

typedef struct {
    const elf_loader_symbol_t *exported; /*!< Pointer to exported symbols array */
    unsigned int exported_size;          /*!< Elements on exported symbol array */
} elf_loader_env_t;

typedef struct elf_loader_ctx_s elf_loader_ctx_t;

int elf_load(LOADER_FD_T fd, const elf_loader_env_t *env, char *funcname, int arg);
intptr_t elf_loader_run(elf_loader_ctx_t *ctx, intptr_t arg);
int elf_loader_set_function(elf_loader_ctx_t *ctx, const char *funcname);
elf_loader_ctx_t *elf_loader_init_load_and_relocate(LOADER_FD_T fd, const elf_loader_env_t *env);
void elf_loader_free(elf_loader_ctx_t *ctx);
void *elf_loader_get_text_addr(elf_loader_ctx_t *ctx);


#endif//MAIN_ELF_LOADER_H
