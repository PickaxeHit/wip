//
// Created by pickaxehit on 2023/7/11.
//

#ifndef MAIN_UNALIGNED_H
#define MAIN_UNALIGNED_H

#include "esp_attr.h"
#include <stddef.h>
#include <stdint.h>

IRAM_ATTR void unaligned_copy(void *dest, void *src, size_t size);
IRAM_ATTR void unaligned_set32(void *dest, uint32_t value);
IRAM_ATTR uint32_t unaligned_get32(void *src);
IRAM_ATTR void unaligned_set8(void *dest, uint8_t value);
IRAM_ATTR uint8_t unaligned_get8(void *src);

#endif//MAIN_UNALIGNED_H
