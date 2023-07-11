//
// Created by pickaxehit on 2023/7/11.
//

#ifndef MAIN_ELF_LOADER_H
#define MAIN_ELF_LOADER_H

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

#endif//MAIN_ELF_LOADER_H
