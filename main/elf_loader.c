//
// Created by pickaxehit on 2023/7/11.
//

#include "elf_loader.h"
#include "elf.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mmu_map.h"
#include "unaligned.h"
#include <esp_check.h>
#include <stddef.h>
#include <sys/types.h>

static const char *TAG = "elf_loader";

typedef struct elf_loader_section_s {
    void *data;
    Elf32_Half secidx;
    size_t size;
    Elf32_Half relsecidx;
    struct elf_loader_section_s *next;
} elf_loader_section_t;


struct elf_loader_ctx_s {
    LOADER_FD_T fd;
    void *exec;
    void *text;
    const elf_loader_env_t *env;

    Elf32_Half e_shnum;
    Elf32_Off e_shoff;
    Elf32_Off shstrtab_offset;

    Elf32_Word symtab_count;
    Elf32_Off symtab_offset;
    Elf32_Off strtab_offset;

    elf_loader_section_t *section;
};

IRAM_ATTR void *malloc_psram_executable(size_t size) {
    //Malloc from PSRAM
    void *raw_buffer = NULL;
    raw_buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (raw_buffer == NULL) {
        ESP_LOGE(TAG, "No mem for psram.");
        return NULL;
    }

    //Get the physical address for allocated memory
    esp_paddr_t psram_buf_paddr = 0;
    mmu_target_t out_target;
    ESP_ERROR_CHECK(esp_mmu_vaddr_to_paddr(raw_buffer, &psram_buf_paddr, &out_target));

    //Map the same physical pages to instruction bus
    const size_t low_paddr = psram_buf_paddr & ~(CONFIG_MMU_PAGE_SIZE - 1);// round down to page boundary
    const size_t high_paddr = (psram_buf_paddr + size + CONFIG_MMU_PAGE_SIZE - 1) &
                              ~(CONFIG_MMU_PAGE_SIZE - 1);// round up to page boundary
    const size_t map_size = high_paddr - low_paddr;
    void *mmap_ptr = NULL;
    ESP_ERROR_CHECK(esp_mmu_map(0, map_size, MMU_TARGET_PSRAM0, MMU_MEM_CAP_EXEC, 0, &mmap_ptr));
    esp_mmu_map_dump_mapped_blocks(stdout);

    //Adjust the mapped pointer to point to the beginning of the buffer
    void *exec_buf = mmap_ptr + (psram_buf_paddr - low_paddr);
    return exec_buf;
}

//#define LOADER_GETDATA(ctx, offset, buffer, size) unaligned_copy(buffer, ctx->fd + offset, size)
static inline void loader_get_data(elf_loader_ctx_t *ctx, uintptr_t offset, void *buffer, size_t size) {
    unaligned_copy(buffer, ctx->fd + offset, size);
}

static inline void *loader_alloc_exec(size_t size) {
    return malloc_psram_executable(size);
}

static inline void *loader_alloc_data(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}

static inline void *data_vaddr_to_instr_vaddr(void *vaddr) {
    esp_paddr_t paddr = 0;
    mmu_target_t target;
    void *out_vaddr = NULL;
    ESP_ERROR_CHECK(esp_mmu_vaddr_to_paddr(vaddr, &paddr, &target));
    ESP_ERROR_CHECK(esp_mmu_paddr_to_vaddr(paddr, target, MMU_VADDR_INSTRUCTION, &vaddr));
    return out_vaddr;
}

static inline void *instr_vaddr_to_data_vaddr(void *vaddr) {
    esp_paddr_t paddr = 0;
    mmu_target_t target;
    void *out_vaddr = NULL;
    ESP_ERROR_CHECK(esp_mmu_vaddr_to_paddr(vaddr, &paddr, &target));
    ESP_ERROR_CHECK(esp_mmu_paddr_to_vaddr(paddr, target, MMU_VADDR_DATA, &vaddr));
    return out_vaddr;
}

//read section header in ctx->fd to "section_header" and section name to "name"
static int loader_read_section(elf_loader_ctx_t *ctx, Elf32_Half secidx, Elf32_Shdr *section_header, char *name,
                               size_t name_len) {
    Elf32_Off offset = ctx->e_shoff + secidx * sizeof(Elf32_Shdr);
    loader_get_data(ctx, offset, section_header, sizeof(Elf32_Shdr));

    if (section_header->sh_name) {
        offset = ctx->shstrtab_offset + (section_header->sh_name);
        loader_get_data(ctx, offset, name, name_len);
    }
    return 0;
}

//read symbol in ctx->fd to "sym" and symbol name to "name"
static int loader_read_symbol(elf_loader_ctx_t *ctx, Elf32_Half symidx, Elf32_Sym *sym, char *name, size_t name_len) {
    Elf32_Off sym_off = ctx->symtab_offset + symidx * sizeof(Elf32_Sym);
    loader_get_data(ctx, sym_off, sym, sizeof(Elf32_Sym));
    if (sym->st_name) {
        Elf32_Off str_off = ctx->strtab_offset + sym->st_name;
        loader_get_data(ctx, str_off, name, name_len);
    } else {
        Elf32_Shdr section_header;
        return loader_read_section(ctx, sym->st_shndx, &section_header, name, name_len);
    }
    return 0;
}

static const char *type_to_string(uint8_t symbol_type) {
    switch (symbol_type) {
        case R_XTENSA_NONE:
            return "R_XTENSA_NONE";
        case R_XTENSA_32:
            return "R_XTENSA_32";
        case R_XTENSA_ASM_EXPAND:
            return "R_XTENSA_ASM_EXPAND";
        case R_XTENSA_SLOT0_OP:
            return "R_XTENSA_SLOT0_OP";
        default:
            return "R_<unknow>";
    }
}

static int loader_relocate_symbol(Elf32_Addr rel_addr, uint8_t symbol_type, Elf32_Addr symbol_addr, Elf32_Addr def_addr,
                                  uint32_t *from, uint32_t *to) {
    if (symbol_addr == 0xffffffff) {
        if (def_addr == 0x00000000) {
            ESP_LOGE(TAG, "Relocation: undefined symbol_addr");
            return -1;
        } else {
            symbol_addr = def_addr;
        }
    }
    switch (symbol_type) {
        case R_XTENSA_32: {
            *from = unaligned_get32((void *) rel_addr);
            *to = symbol_addr + *from;
            unaligned_set32((void *) rel_addr, *to);
            break;
        }
        case R_XTENSA_SLOT0_OP: {
            uint32_t v = unaligned_get32((void *) rel_addr);
            *from = v;

            /* *** Format: L32R *** */
            if ((v & 0x00000F) == 0x000001) {
                int32_t delta = symbol_addr - ((rel_addr + 3) & 0xfffffffc);
                if (delta & 0x0000003) {
                    ESP_LOGE(TAG, "Relocation: L32R error");
                    return -1;
                }
                delta = delta >> 2;
                unaligned_set8((void *) (rel_addr + 1), ((uint8_t *) &delta)[0]);
                unaligned_set8((void *) (rel_addr + 2), ((uint8_t *) &delta)[1]);
                *to = unaligned_get32((void *) rel_addr);
                break;
            }

            /* *** Format: CALL *** */
            /* *** CALL0, CALL4, CALL8, CALL12, J *** */
            if ((v & 0x00000F) == 0x000005) {
                int32_t delta = symbol_addr - ((rel_addr + 4) & 0xfffffffc);
                if (delta & 0x0000003) {
                    ESP_LOGE(TAG, "Relocation: CALL error");
                    return -1;
                }
                delta = delta >> 2;
                delta = delta << 6;
                delta |= unaligned_get8((void *) (rel_addr + 0));
                unaligned_set8((void *) (rel_addr + 0), ((uint8_t *) &delta)[0]);
                unaligned_set8((void *) (rel_addr + 1), ((uint8_t *) &delta)[1]);
                unaligned_set8((void *) (rel_addr + 2), ((uint8_t *) &delta)[2]);
                *to = unaligned_get32((void *) rel_addr);
                break;
            }

            /* *** J *** */
            if ((v & 0x00003F) == 0x000006) {
                int32_t delta = symbol_addr - (rel_addr + 4);
                delta = delta << 6;
                delta |= unaligned_get8((void *) (rel_addr + 0));
                unaligned_set8((void *) (rel_addr + 0), ((uint8_t *) &delta)[0]);
                unaligned_set8((void *) (rel_addr + 1), ((uint8_t *) &delta)[1]);
                unaligned_set8((void *) (rel_addr + 2), ((uint8_t *) &delta)[2]);
                *to = unaligned_get32((void *) rel_addr);
                break;
            }

            /* *** Format: BRI8  *** */
            /* *** BALL, BANY, BBC, BBCI, BBCI.L, BBS,  BBSI, BBSI.L, BEQ, BGE,  BGEU, BLT, BLTU, BNALL, BNE,  BNONE, LOOP,  *** */
            /* *** BEQI, BF, BGEI, BGEUI, BLTI, BLTUI, BNEI,  BT, LOOPGTZ, LOOPNEZ *** */
            if (((v & 0x00000F) == 0x000007) || ((v & 0x00003F) == 0x000026) ||
                ((v & 0x00003F) == 0x000036 && (v & 0x0000FF) != 0x000036)) {
                int32_t delta = symbol_addr - (rel_addr + 4);
                unaligned_set8((void *) (rel_addr + 2), ((uint8_t *) &delta)[0]);
                *to = unaligned_get32((void *) rel_addr);
                if ((delta < -(1 << 7)) || (delta >= (1 << 7))) {
                    ESP_LOGE(TAG, "Relocation: BRI8 out of range");
                    return -1;
                }
                break;
            }

            /* *** Format: BRI12 *** */
            /* *** BEQZ, BGEZ, BLTZ, BNEZ *** */
            if ((v & 0x00003F) == 0x000016) {
                int32_t delta = symbol_addr - (rel_addr + 4);
                delta = delta << 4;
                delta |= unaligned_get32((void *) (rel_addr + 1));
                unaligned_set8((void *) (rel_addr + 1), ((uint8_t *) &delta)[0]);
                unaligned_set8((void *) (rel_addr + 2), ((uint8_t *) &delta)[1]);
                *to = unaligned_get32((void *) rel_addr);
                delta = symbol_addr - (rel_addr + 4);
                if ((delta < -(1 << 11)) || (delta >= (1 << 11))) {
                    ESP_LOGE(TAG, "Relocation: BRI12 out of range");
                    return -1;
                }
                break;
            }

            /* *** Format: RI6  *** */
            /* *** BEQZ.N, BNEZ.N *** */
            if ((v & 0x008F) == 0x008C) {
                int32_t delta = symbol_addr - (rel_addr + 4);
                int32_t d2 = delta & 0x30;
                int32_t d1 = (delta << 4) & 0xf0;
                d2 |= unaligned_get32((void *) (rel_addr + 0));
                d1 |= unaligned_get32((void *) (rel_addr + 1));
                unaligned_set8((void *) (rel_addr + 0), ((uint8_t *) &d2)[0]);
                unaligned_set8((void *) (rel_addr + 1), ((uint8_t *) &d1)[0]);
                *to = unaligned_get32((void *) rel_addr);
                if ((delta < 0) || (delta > 0x111111)) {
                    ESP_LOGE(TAG, "Relocation: RI6 out of range");
                    return -1;
                }
                break;
            }

            ESP_LOGE(TAG, "Relocation: unknown opcode %08X", v);
            return -1;
            break;
        }
        case R_XTENSA_ASM_EXPAND: {
            *from = unaligned_get32((void *) rel_addr);
            *to = unaligned_get32((void *) rel_addr);
            break;
        }
        default:
            ESP_LOGI(TAG, "Relocation: undefined relocation %d %s", symbol_type, type_to_string(symbol_type));
            assert(0);
            return -1;
    }
    return 0;
}