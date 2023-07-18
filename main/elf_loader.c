//
// Created by pickaxehit on 2023/7/11.
//

#include "elf_loader.h"
#include "elf.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mmu_map.h"
#include "unaligned.h"
#include <esp_check.h>
#include <stddef.h>
#include <string.h>
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
    return raw_buffer;
}

//#define LOADER_GETDATA(ctx, offset, buffer, size) unaligned_copy(buffer, ctx->fd + offset, size)
static inline void loader_get_data(elf_loader_ctx_t *ctx, uintptr_t offset, void *buffer, size_t size) {
    unaligned_copy(buffer, ctx->fd + offset, size);
}

static inline void loader_get_data_no_alignment(elf_loader_ctx_t *ctx, uintptr_t offset, void *buffer, size_t size) {
    memcpy(buffer, ctx->fd + offset, size);
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
    ESP_ERROR_CHECK(esp_mmu_paddr_to_vaddr(paddr, target, MMU_VADDR_INSTRUCTION, &out_vaddr));
    return out_vaddr;
}

static inline void *instr_vaddr_to_data_vaddr(void *vaddr) {
    esp_paddr_t paddr = 0;
    mmu_target_t target;
    void *out_vaddr = NULL;
    ESP_ERROR_CHECK(esp_mmu_vaddr_to_paddr(vaddr, &paddr, &target));
    ESP_ERROR_CHECK(esp_mmu_paddr_to_vaddr(paddr, target, MMU_VADDR_DATA, &out_vaddr));
    return out_vaddr;
}

//read section header in ctx->fd to "section_header" and section name to "name"
static int loader_read_section(elf_loader_ctx_t *ctx, Elf32_Half secidx, Elf32_Shdr *section_header, char *name,
                               size_t name_len) {
    Elf32_Off offset = ctx->e_shoff + secidx * sizeof(Elf32_Shdr);
    loader_get_data_no_alignment(ctx, offset, section_header, sizeof(Elf32_Shdr));

    if (section_header->sh_name) {
        offset = ctx->shstrtab_offset + (section_header->sh_name);
        loader_get_data_no_alignment(ctx, offset, name, name_len);
    }
    return 0;
}

//read symbol in ctx->fd to "sym" and symbol name to "name"
static int loader_read_symbol(elf_loader_ctx_t *ctx, Elf32_Half symidx, Elf32_Sym *sym, char *name, size_t name_len) {
    Elf32_Off sym_off = ctx->symtab_offset + symidx * sizeof(Elf32_Sym);
    loader_get_data_no_alignment(ctx, sym_off, sym, sizeof(Elf32_Sym));
    if (sym->st_name) {
        Elf32_Off str_off = ctx->strtab_offset + sym->st_name;
        loader_get_data_no_alignment(ctx, str_off, name, name_len);
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
                                  uint32_t *from, uint32_t *to /*, uint8_t cache_sync*/) {
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
            //*from = unaligned_get32((void *) rel_addr);
            *from = *(uint32_t *) rel_addr;
            *to = symbol_addr + *from;
            //unaligned_set32((void *) rel_addr, *to);
            *(uint32_t *) rel_addr = *to;
            //if (cache_sync) {
            esp_cache_msync((void *) rel_addr, 4, ESP_CACHE_MSYNC_FLAG_UNALIGNED);
            //}
            break;
        }
        case R_XTENSA_SLOT0_OP: {
            //uint32_t value = unaligned_get32((void *) rel_addr);
            uint32_t value = *(uint32_t *) rel_addr;
            *from = value;

            /* *** Format: L32R *** */
            if ((value & 0x00000F) == 0x000001) {
                int32_t delta = symbol_addr - ((rel_addr + 3) & 0xfffffffc);
                if (delta & 0x0000003) {
                    ESP_LOGE(TAG, "Relocation: L32R error");
                    return -1;
                }
                delta = delta >> 2;
                //unaligned_set8((void *) (rel_addr + 1), ((uint8_t *) &delta)[0]);
                //unaligned_set8((void *) (rel_addr + 2), ((uint8_t *) &delta)[1]);
                //*(uint8_t *) (rel_addr + 1) = ((uint8_t *) &delta)[0];
                //*(uint8_t *) (rel_addr + 2) = ((uint8_t *) &delta)[1];
                memcpy((void *) (rel_addr + 1), (void *) &delta, 2);
                //if (cache_sync) {
                esp_cache_msync((void *) (rel_addr + 1), 2, ESP_CACHE_MSYNC_FLAG_UNALIGNED);
                //}
                //*to = unaligned_get32((void *) rel_addr);
                *to = *(uint32_t *) rel_addr;
                break;
            }

            /* *** Format: CALL *** */
            /* *** CALL0, CALL4, CALL8, CALL12, J *** */
            if ((value & 0x00000F) == 0x000005) {
                int32_t delta = symbol_addr - ((rel_addr + 4) & 0xfffffffc);
                if (delta & 0x0000003) {
                    ESP_LOGE(TAG, "Relocation: CALL error");
                    return -1;
                }
                delta = delta >> 2;
                delta = delta << 6;
                //delta |= unaligned_get8((void *) (rel_addr + 0));
                delta |= *(uint8_t *) (rel_addr + 0);
                //unaligned_set8((void *) (rel_addr + 0), ((uint8_t *) &delta)[0]);
                //unaligned_set8((void *) (rel_addr + 1), ((uint8_t *) &delta)[1]);
                //unaligned_set8((void *) (rel_addr + 2), ((uint8_t *) &delta)[2]);
                memcpy((void *) rel_addr, (void *) &delta, 3);
                //if (cache_sync) {
                esp_cache_msync((void *) rel_addr, 3, ESP_CACHE_MSYNC_FLAG_UNALIGNED);
                //}
                //*to = unaligned_get32((void *) rel_addr);
                *to = *(uint32_t *) rel_addr;
                break;
            }

            /* *** J *** */
            if ((value & 0x00003F) == 0x000006) {
                int32_t delta = symbol_addr - (rel_addr + 4);
                delta = delta << 6;
                //delta |= unaligned_get8((void *) (rel_addr + 0));
                delta |= *(uint8_t *) (rel_addr + 0);
                //unaligned_set8((void *) (rel_addr + 0), ((uint8_t *) &delta)[0]);
                //unaligned_set8((void *) (rel_addr + 1), ((uint8_t *) &delta)[1]);
                //unaligned_set8((void *) (rel_addr + 2), ((uint8_t *) &delta)[2]);
                memcpy((void *) rel_addr, (void *) &delta, 3);
                //if (cache_sync) {
                esp_cache_msync((void *) rel_addr, 3, ESP_CACHE_MSYNC_FLAG_UNALIGNED);
                //}
                //*to = unaligned_get32((void *) rel_addr);
                *to = *(uint32_t *) rel_addr;
                break;
            }

            /* *** Format: BRI8  *** */
            /* *** BALL, BANY, BBC, BBCI, BBCI.L, BBS,  BBSI, BBSI.L, BEQ, BGE,  BGEU, BLT, BLTU, BNALL, BNE,  BNONE, LOOP,  *** */
            /* *** BEQI, BF, BGEI, BGEUI, BLTI, BLTUI, BNEI,  BT, LOOPGTZ, LOOPNEZ *** */
            if (((value & 0x00000F) == 0x000007) || ((value & 0x00003F) == 0x000026) ||
                ((value & 0x00003F) == 0x000036 && (value & 0x0000FF) != 0x000036)) {
                int32_t delta = symbol_addr - (rel_addr + 4);
                //unaligned_set8((void *) (rel_addr + 2), ((uint8_t *) &delta)[0]);
                *(uint8_t *) (rel_addr + 2) = *(uint8_t *) &delta;
                //if (cache_sync) {
                esp_cache_msync((void *) (rel_addr + 2), 1, ESP_CACHE_MSYNC_FLAG_UNALIGNED);
                //}
                //*to = unaligned_get32((void *) rel_addr);
                *to = *(uint32_t *) rel_addr;
                if ((delta < -(1 << 7)) || (delta >= (1 << 7))) {
                    ESP_LOGE(TAG, "Relocation: BRI8 out of range");
                    return -1;
                }
                break;
            }

            /* *** Format: BRI12 *** */
            /* *** BEQZ, BGEZ, BLTZ, BNEZ *** */
            if ((value & 0x00003F) == 0x000016) {
                int32_t delta = symbol_addr - (rel_addr + 4);
                delta = delta << 4;
                //delta |= unaligned_get32((void *) (rel_addr + 1));
                delta |= *(uint8_t *) (rel_addr + 1);
                //unaligned_set8((void *) (rel_addr + 1), ((uint8_t *) &delta)[0]);
                //unaligned_set8((void *) (rel_addr + 2), ((uint8_t *) &delta)[1]);
                memcpy((void *) (rel_addr + 1), (void *) &delta, 2);
                //if (cache_sync) {
                esp_cache_msync((void *) (rel_addr + 1), 2, ESP_CACHE_MSYNC_FLAG_UNALIGNED);
                //}
                //*to = unaligned_get32((void *) rel_addr);
                *to = *(uint32_t *) rel_addr;
                delta = symbol_addr - (rel_addr + 4);
                if ((delta < -(1 << 11)) || (delta >= (1 << 11))) {
                    ESP_LOGE(TAG, "Relocation: BRI12 out of range");
                    return -1;
                }
                break;
            }

            /* *** Format: RI6  *** */
            /* *** BEQZ.N, BNEZ.N *** */
            if ((value & 0x008F) == 0x008C) {
                int32_t delta = symbol_addr - (rel_addr + 4);
                int32_t d2 = delta & 0x30;
                int32_t d1 = (delta << 4) & 0xf0;
                //d2 |= unaligned_get32((void *) (rel_addr + 0));
                //d1 |= unaligned_get32((void *) (rel_addr + 1));
                d2 |= *(uint32_t *) (rel_addr + 0);
                d1 |= *(uint32_t *) (rel_addr + 1);
                //unaligned_set8((void *) (rel_addr + 0), ((uint8_t *) &d2)[0]);
                //unaligned_set8((void *) (rel_addr + 1), ((uint8_t *) &d1)[0]);
                *(uint8_t *) (rel_addr + 0) = *(uint8_t *) &d2;
                *(uint8_t *) (rel_addr + 1) = *(uint8_t *) &d1;
                //if (cache_sync) {
                esp_cache_msync((void *) rel_addr, 2, ESP_CACHE_MSYNC_FLAG_UNALIGNED);
                //}
                //*to = unaligned_get32((void *) rel_addr);
                *to = *(uint32_t *) rel_addr;
                if ((delta < 0) || (delta > 0x111111)) {
                    ESP_LOGE(TAG, "Relocation: RI6 out of range");
                    return -1;
                }
                break;
            }

            ESP_LOGE(TAG, "Relocation: unknown opcode %08lX", value);
            return -1;
            break;
        }
        case R_XTENSA_ASM_EXPAND: {
            //*from = unaligned_get32((void *) rel_addr);
            *from = *(uint32_t *) rel_addr;
            //*to = unaligned_get32((void *) rel_addr);
            *to = *(uint32_t *) rel_addr;
            break;
        }
        default:
            ESP_LOGI(TAG, "Relocation: undefined relocation %d %s", symbol_type, type_to_string(symbol_type));
            assert(0);
            return -1;
    }
    return 0;
}

void elf_loader_free(elf_loader_ctx_t *ctx) {
    if (ctx) {
        elf_loader_section_t *section = ctx->section;
        elf_loader_section_t *next;
        while (section != NULL) {
            if (section->data) {
                free(section->data);
            }
            next = section->next;
            free(section);
            section = next;
        }
        free(ctx);
    }
}


static elf_loader_section_t *loader_find_section(elf_loader_ctx_t *ctx, int index) {
    for (elf_loader_section_t *section = ctx->section; section != NULL; section = section->next) {
        if (section->secidx == index) {
            return section;
        }
    }
    return NULL;
}


static Elf32_Addr loader_find_symbol_addr(elf_loader_ctx_t *ctx, Elf32_Sym *sym, const char *sName) {
    for (int i = 0; i < ctx->env->exported_size; i++) {
        if (strcmp(ctx->env->exported[i].name, sName) == 0) {
            return (Elf32_Addr) (ctx->env->exported[i].ptr);
        }
    }
    elf_loader_section_t *symbol_section = loader_find_section(ctx, sym->st_shndx);
    if (symbol_section)
        return ((Elf32_Addr) symbol_section->data) + sym->st_value;
    return 0xffffffff;
}


static int loader_relocate_section(elf_loader_ctx_t *ctx, elf_loader_section_t *section) {
    char name[33] = "<unamed>";
    Elf32_Shdr sectHdr;
    if (loader_read_section(ctx, section->relsecidx, &sectHdr, name, sizeof(name)) != 0) {
        ESP_LOGE(TAG, "Error reading section header");
        return -1;
    }
    if (!(section->relsecidx)) {
        ESP_LOGI(TAG, "  Section %section: no relocation index", name);
        return 0;
    }
    if (!(section->data)) {
        ESP_LOGE(TAG, "Section not loaded: %section", name);
        return -1;
    }

    ESP_LOGI(TAG, "  Section %section", name);
    int r = 0;
    Elf32_Rela rel;
    size_t rel_entries = sectHdr.sh_size / sizeof(rel);
    ESP_LOGI(TAG,
             "  Offset   Sym  Type                      relAddr  symAddr  defValue                    Name + addend");
    for (size_t rel_count = 0; rel_count < rel_entries; rel_count++) {
        loader_get_data_no_alignment(ctx, sectHdr.sh_offset + rel_count * (sizeof(rel)), &rel, sizeof(rel));
        Elf32_Sym sym;
        char name[33] = "<unnamed>";
        int symbol_entry = ELF32_R_SYM(rel.r_info);
        int rel_type = ELF32_R_TYPE(rel.r_info);
        Elf32_Addr rel_addr = ((Elf32_Addr) section->data) + rel.r_offset;// data to be updated adress
        loader_read_symbol(ctx, symbol_entry, &sym, name, sizeof(name));
        Elf32_Addr symbol_addr = loader_find_symbol_addr(ctx, &sym, name) + rel.r_addend;// target symbol adress
        uint32_t from = 0;
        uint32_t to = 0;
        if (rel_type == R_XTENSA_NONE || rel_type == R_XTENSA_ASM_EXPAND) {
            //            ESP_LOGI(TAG,"  %08X %04X %04X %-20s %08X          %08X                    %section + %X", rel.r_offset, symbol_entry, rel_type, type2String(rel_type), rel_addr, sym.st_value, name, rel.r_addend);
        } else if ((symbol_addr == 0xffffffff) && (sym.st_value == 0x00000000)) {
            ESP_LOGE(TAG, "Relocation - undefined symbol_addr: %section", name);
            ESP_LOGI(TAG, "  %08lX %04X %04X %-20s %08lX %08lX %08lX                    %section + %lX", rel.r_offset,
                     symbol_entry, rel_type, type_to_string(rel_type), rel_addr, symbol_addr, sym.st_value, name,
                     rel.r_addend);
            r = -1;
        } else if (loader_relocate_symbol(rel_addr, rel_type, symbol_addr, sym.st_value, &from, &to) != 0) {
            ESP_LOGE(TAG, "  %08lX %04X %04X %-20s %08lX %08lX %08lX %08lX->%08lX %section + %lX", rel.r_offset,
                     symbol_entry, rel_type, type_to_string(rel_type), rel_addr, symbol_addr, sym.st_value, from, to,
                     name, rel.r_addend);
            r = -1;
        } else {
            ESP_LOGI(TAG, "  %08lX %04X %04X %-20s %08lX %08lX %08lX %08lX->%08lX %section + %lX", rel.r_offset,
                     symbol_entry, rel_type, type_to_string(rel_type), rel_addr, symbol_addr, sym.st_value, from, to,
                     name, rel.r_addend);
        }
    }
    return r;
}

elf_loader_ctx_t *elf_loader_init_load_and_relocate(LOADER_FD_T fd, const elf_loader_env_t *env) {
    ESP_LOGI(TAG, "ENV:");
    for (int i = 0; i < env->exported_size; i++) {
        ESP_LOGI(TAG, "  %08X %section", (unsigned int) env->exported[i].ptr, env->exported[i].name);
    }

    elf_loader_ctx_t *ctx = malloc(sizeof(elf_loader_ctx_t));
    assert(ctx);

    memset(ctx, 0, sizeof(elf_loader_ctx_t));
    ctx->fd = fd;
    ctx->env = env;
    {
        Elf32_Ehdr header;
        Elf32_Shdr section;
        /* Load the ELF header, located at the start of the buffer. */
        loader_get_data_no_alignment(ctx, 0, &header, sizeof(Elf32_Ehdr));

        /* Make sure that we have a correct and compatible ELF header. */
        char elf_magic[] = {0x7f, 'E', 'L', 'F', '\0'};
        if (memcmp(header.e_ident, elf_magic, strlen(elf_magic)) != 0) {
            ESP_LOGE(TAG, "Bad ELF Identification");
            goto err;
        }

        /* Load the section header, get the number of entries of the section header, get a pointer to the actual table of strings */
        loader_get_data_no_alignment(ctx, header.e_shoff + header.e_shstrndx * sizeof(Elf32_Shdr), &section,
                                     sizeof(Elf32_Shdr));
        ctx->e_shnum = header.e_shnum;
        ctx->e_shoff = header.e_shoff;
        ctx->shstrtab_offset = section.sh_offset;
    }

    {
        /* Go through all sections, allocate and copy the relevant ones
       ".symtab": segment contains the symbol table for this file
       ".strtab": segment points to the actual string names used by the symbol table
       */
        ESP_LOGI(TAG, "Scanning ELF sections         relAddr      size");
        for (int n = 1; n < ctx->e_shnum; n++) {
            Elf32_Shdr section_header;
            char name[33] = "<unamed>";
            if (loader_read_section(ctx, n, &section_header, name, sizeof(name)) != 0) {
                ESP_LOGE(TAG, "Error reading section");
                goto err;
            }
            if (section_header.sh_flags & SHF_ALLOC) {
                if (!section_header.sh_size) {
                    ESP_LOGI(TAG, "  section %2d: %-15s no data", n, name);
                } else {
                    elf_loader_section_t *section = malloc(sizeof(elf_loader_section_t));
                    assert(section);
                    memset(section, 0, sizeof(elf_loader_section_t));
                    section->next = ctx->section;
                    ctx->section = section;
                    if (section_header.sh_flags & SHF_EXECINSTR) {
                        section->data = loader_alloc_exec(section_header.sh_size);
                    } else {
                        section->data = loader_alloc_data(section_header.sh_size);
                    }
                    if (!section->data) {
                        ESP_LOGE(TAG, "Section malloc failled: %section", name);
                        goto err;
                    }
                    section->secidx = n;
                    section->size = section_header.sh_size;
                    if (section_header.sh_type != SHT_NOBITS) {
                        loader_get_data_no_alignment(ctx, section_header.sh_offset, section->data,
                                                     section_header.sh_size);
                    }
                    if (strcmp(name, ".text") == 0) {
                        ctx->text = section->data;
                    }
                    ESP_LOGI(TAG, "  section %2d: %-15s %08X %6lu", n, name, (unsigned int) section->data,
                             section_header.sh_size);
                }
            } else if (section_header.sh_type == SHT_RELA) {
                if (section_header.sh_info >= n) {
                    ESP_LOGE(TAG, "Rela section: bad linked section (%i:%section -> %lu)", n, name,
                             section_header.sh_info);
                    goto err;
                }
                elf_loader_section_t *section = loader_find_section(ctx, section_header.sh_info);
                if (section == NULL) {
                    ESP_LOGI(TAG, "  section %2d: %-15s -> %2lu: ignoring", n, name, section_header.sh_info);
                } else {
                    section->relsecidx = n;
                    ESP_LOGI(TAG, "  section %2d: %-15s -> %2lu: ok", n, name, section_header.sh_info);
                }
            } else {
                ESP_LOGI(TAG, "  section %2d: %section", n, name);
                if (strcmp(name, ".symtab") == 0) {
                    ctx->symtab_offset = section_header.sh_offset;
                    ctx->symtab_count = section_header.sh_size / sizeof(Elf32_Sym);
                } else if (strcmp(name, ".strtab") == 0) {
                    ctx->strtab_offset = section_header.sh_offset;
                }
            }
        }
        if (ctx->symtab_offset == 0 || ctx->symtab_offset == 0) {
            ESP_LOGE(TAG, "Missing .symtab or .strtab section");
            goto err;
        }
    }

    {
        ESP_LOGI(TAG, "Relocating sections");
        int r = 0;
        for (elf_loader_section_t *section = ctx->section; section != NULL; section = section->next) {
            r |= loader_relocate_section(ctx, section);
        }
        if (r != 0) {
            ESP_LOGI(TAG, "Relocation failed");
            goto err;
        }
    }
    return ctx;

err:
    elf_loader_free(ctx);
    return NULL;
}


int elf_loader_set_function(elf_loader_ctx_t *ctx, const char *funcname) {
    ctx->exec = 0;
    ESP_LOGI(TAG, "Scanning ELF symbols");
    ESP_LOGI(TAG, "  Sym  Symbol                         sect value    size relAddr");
    for (int symbol_count = 0; symbol_count < ctx->symtab_count; symbol_count++) {
        Elf32_Sym sym;
        char name[33] = "<unnamed>";
        if (loader_read_symbol(ctx, symbol_count, &sym, name, sizeof(name)) != 0) {
            ESP_LOGE(TAG, "Error reading symbol");
            return -1;
        }
        if (strcmp(name, funcname) == 0) {
            Elf32_Addr symbol_addr = loader_find_symbol_addr(ctx, &sym, name);
            if (symbol_addr == 0xffffffff) {
                ESP_LOGI(TAG, "  %04X %-30s %04X %08lX %04lX ????????", symbol_count, name, sym.st_shndx, sym.st_value,
                         sym.st_size);
            } else {
                //ctx->exec = (void *) symbol_addr;
                ctx->exec = data_vaddr_to_instr_vaddr((void *) symbol_addr);
                ESP_LOGI(TAG, "  %04X %-30s %04X %08lX %04lX %08lX", symbol_count, name, sym.st_shndx, sym.st_value,
                         sym.st_size, symbol_addr);
            }
        } else {
            ESP_LOGI(TAG, "  %04X %-30s %04X %08lX %04lX", symbol_count, name, sym.st_shndx, sym.st_value, sym.st_size);
        }
    }
    if (ctx->exec == 0) {
        ESP_LOGE(TAG, "Function symbol not found: %section", funcname);
        return -1;
    }
    return 0;
}


intptr_t elf_loader_run(elf_loader_ctx_t *ctx, intptr_t arg) {
    if (!ctx->exec) {
        return 0;
    }
    typedef int (*func_t)(int);
    func_t func = (func_t) ctx->exec;
    ESP_LOGI(TAG, "Running...");
    int r = func(arg);
    ESP_LOGI(TAG, "Result: %08X", r);
    return r;
}


int elf_load(LOADER_FD_T fd, const elf_loader_env_t *env, char *funcname, int arg) {
    elf_loader_ctx_t *ctx = elf_loader_init_load_and_relocate(fd, env);
    if (!ctx) {
        return -1;
    }
    if (elf_loader_set_function(ctx, funcname) != 0) {
        elf_loader_free(ctx);
        return -1;
    }
    int r = elf_loader_run(ctx, arg);
    elf_loader_free(ctx);
    return r;
}

void *elf_loader_get_text_addr(elf_loader_ctx_t *ctx) {
    return ctx->text;
}
