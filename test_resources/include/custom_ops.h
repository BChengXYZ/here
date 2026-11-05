#ifndef CUSTOM_OPS_H
#define CUSTOM_OPS_H

#include <stdint.h>

static inline uint64_t padd8(uint64_t a, uint64_t b) {
    uint64_t result;
    asm volatile(
        ".insn r 0x0b, 0, 0, %0, %1, %2"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

static inline int64_t pdot8(uint64_t a, uint64_t b) {
    int64_t result;
    asm volatile(
        ".insn r 0x0b, 1, 0, %0, %1, %2"
        : "=r"(result)
        : "r"(a), "r"(b)
    );
    return result;
}

#endif
