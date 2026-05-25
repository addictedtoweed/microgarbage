/* ============================================================
 *  bits.h — bit-scan primitives (clz / ctz / popcount).
 *
 *  Platform-tailored, header-only. On GCC/Clang these compile to the
 *  hardware instruction: on the Cortex-M (H745) `bits_clz*` is the CLZ
 *  instruction and `bits_ctz*` is RBIT+CLZ; on x86 they're BSR/BSF/
 *  POPCNT. With no compiler builtins available, portable software
 *  fallbacks are used (fine — these sit in cold paths like allocator
 *  setup and bitmap free-slot scans, not inner loops).
 *
 *  Zero input is DEFINED here (the raw CLZ/CTZ instructions and the
 *  __builtin_* forms leave clz(0)/ctz(0) undefined): bits_clz*(0)
 *  returns the full width (32 / 64) and bits_ctz*(0) likewise. That's
 *  what a bitmap scanner wants — "no set bit" reads as "scanned the
 *  whole word."
 *
 *    bits_clz32(x)  leading  zeros in a uint32_t  (x==0 -> 32)
 *    bits_ctz32(x)  trailing zeros in a uint32_t  (x==0 -> 32)
 *    bits_popcount32(x)  set bits in a uint32_t
 *  ...and 64-bit variants.
 *
 *  Depends on: nothing.
 *
 *  Public domain (CC0). No warranty.
 *  https://creativecommons.org/publicdomain/zero/1.0/
 * ============================================================ */

#ifndef GARBAGE_MATH_BITS_H
#define GARBAGE_MATH_BITS_H

#include <stdint.h>

/* Auto-detected, but overridable: define GARBAGE_BITS_HAVE_BUILTINS=0
 * before including to force the portable software path even on a
 * compiler that has the builtins (handy for testing the fallback). */
#ifndef GARBAGE_BITS_HAVE_BUILTINS
#  if defined(__GNUC__) || defined(__clang__)
#    define GARBAGE_BITS_HAVE_BUILTINS 1
#  else
#    define GARBAGE_BITS_HAVE_BUILTINS 0
#  endif
#endif

/* ---- leading zeros -------------------------------------------- */

static inline uint8_t bits_clz32(uint32_t x) {
    if (x == 0) return 32;
#if GARBAGE_BITS_HAVE_BUILTINS
    /* __builtin_clz operates on unsigned int (32-bit on our targets). */
    return (uint8_t)__builtin_clz(x);
#else
    uint8_t n = 0;
    if ((x & 0xFFFF0000u) == 0) { n += 16; x <<= 16; }
    if ((x & 0xFF000000u) == 0) { n += 8;  x <<= 8;  }
    if ((x & 0xF0000000u) == 0) { n += 4;  x <<= 4;  }
    if ((x & 0xC0000000u) == 0) { n += 2;  x <<= 2;  }
    if ((x & 0x80000000u) == 0) { n += 1; }
    return n;
#endif
}

static inline uint8_t bits_clz64(uint64_t x) {
    if (x == 0) return 64;
#if GARBAGE_BITS_HAVE_BUILTINS
    return (uint8_t)__builtin_clzll(x);
#else
    if ((x >> 32) == 0) return (uint8_t)(32 + bits_clz32((uint32_t)x));
    return bits_clz32((uint32_t)(x >> 32));
#endif
}

/* ---- trailing zeros ------------------------------------------- */

static inline uint8_t bits_ctz32(uint32_t x) {
    if (x == 0) return 32;
#if GARBAGE_BITS_HAVE_BUILTINS
    return (uint8_t)__builtin_ctz(x);
#else
    uint8_t n = 0;
    if ((x & 0x0000FFFFu) == 0) { n += 16; x >>= 16; }
    if ((x & 0x000000FFu) == 0) { n += 8;  x >>= 8;  }
    if ((x & 0x0000000Fu) == 0) { n += 4;  x >>= 4;  }
    if ((x & 0x00000003u) == 0) { n += 2;  x >>= 2;  }
    if ((x & 0x00000001u) == 0) { n += 1; }
    return n;
#endif
}

static inline uint8_t bits_ctz64(uint64_t x) {
    if (x == 0) return 64;
#if GARBAGE_BITS_HAVE_BUILTINS
    return (uint8_t)__builtin_ctzll(x);
#else
    if ((x & 0xFFFFFFFFu) == 0) return (uint8_t)(32 + bits_ctz32((uint32_t)(x >> 32)));
    return bits_ctz32((uint32_t)x);
#endif
}

/* ---- population count ----------------------------------------- */

static inline uint8_t bits_popcount32(uint32_t x) {
#if GARBAGE_BITS_HAVE_BUILTINS
    return (uint8_t)__builtin_popcount(x);
#else
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    x = (x + (x >> 4)) & 0x0F0F0F0Fu;
    return (uint8_t)((x * 0x01010101u) >> 24);
#endif
}

static inline uint8_t bits_popcount64(uint64_t x) {
#if GARBAGE_BITS_HAVE_BUILTINS
    return (uint8_t)__builtin_popcountll(x);
#else
    return (uint8_t)(bits_popcount32((uint32_t)x) +
                     bits_popcount32((uint32_t)(x >> 32)));
#endif
}

#endif /* GARBAGE_MATH_BITS_H */
