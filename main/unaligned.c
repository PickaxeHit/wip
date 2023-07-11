//
// Created by pickaxehit on 2023/7/11.
//

#include "unaligned.h"


uint8_t unaligned_get8(void *src) {
    uintptr_t csrc = (uintptr_t) src;
    uint32_t v = *(uint32_t *) (csrc & 0xfffffffc);
    v = (v >> (((uint32_t) csrc & 0x3) * 8)) & 0x000000ff;
    return v;
}

void unaligned_set8(void *dest, uint8_t value) {
    uintptr_t cdest = (uintptr_t) dest;
    uint32_t d = *(uint32_t *) (cdest & 0xfffffffc);
    uint32_t v = value;
    v = v << ((cdest & 0x3) * 8);
    d = d & ~(0x000000ff << ((cdest & 0x3) * 8));
    d = d | v;
    *(uint32_t *) (cdest & 0xfffffffc) = d;
}

uint32_t unaligned_get32(void *src) {
    uint32_t d = 0;
    uintptr_t csrc = (uintptr_t) src;
    for (int n = 0; n < 4; n++) {
        uint32_t v = unaligned_get8((void *) csrc);
        v = v << (n * 8);
        d = d | v;
        csrc++;
    }
    return d;
}

void unaligned_set32(void *dest, uint32_t value) {
    uintptr_t cdest = (uintptr_t) dest;
    for (int n = 0; n < 4; n++) {
        unaligned_set8((void *) cdest, value & 0x000000ff);
        value = value >> 8;
        cdest++;
    }
}

void unaligned_copy(void *dest, void *src, size_t size) {
    uintptr_t cdest = (uintptr_t) dest;
    uintptr_t csrc = (uintptr_t) src;
    if (((uintptr_t) dest & 0x3) == 0 && ((uintptr_t) src & 0x3) == 0) {
        uint32_t *adest = (uint32_t *) dest;
        uint32_t *asrc = (uint32_t *) src;
        size_t aligned = size / 4;
        for (size_t i = 0; i < aligned; i++) {
            *(adest + i) = *(asrc + i);
        }
        size_t remaining = size % 4;
        for (size_t i = 0; i < remaining; i++) {
            uint8_t value = unaligned_get8(src + (size - remaining + i));
            unaligned_set8(dest + (size - remaining + i), value);
        }
    } else if (((uintptr_t) dest & 0x3) == ((uintptr_t) src & 0x3)) {
        size_t unaligned = 4 - ((uintptr_t) dest & 0x3);
        if (unaligned > size) {
            unaligned = size;
        }
        uint32_t *adest = (uint32_t *) ((uintptr_t) dest + unaligned);
        uint32_t *asrc = (uint32_t *) ((uintptr_t) src + unaligned);
        for (size_t i = 0; i < unaligned; i++) {
            uint8_t value = unaligned_get8((src + i));
            unaligned_set8(dest + i, value);
        }
        size_t aligned = (size - unaligned) / 4;
        for (size_t i = 0; i < aligned; i++) {
            *(adest + i) = *(asrc + i);
        }
        size_t remaining = (size - unaligned) % 4;
        for (size_t i = 0; i < remaining; i++) {
            uint8_t value = unaligned_get8(src + (size - remaining + i));
            unaligned_set8(dest + (size - remaining + i), value);
        }
    } else {
        for (; size > 0; csrc++, cdest++, size--) {
            uint8_t value = unaligned_get8((void *) csrc);
            unaligned_set8((void *) cdest, value);
        }
    }
}
