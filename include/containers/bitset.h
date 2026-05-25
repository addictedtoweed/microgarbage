/* ============================================================
 *  bitset.h — fixed-size bit set over caller-provided word storage,
 *             with allocator-friendly free-slot scanning.
 *
 *  One reusable home for the "bitmap of N slots" pattern that several
 *  block allocators hand-rolled (audio_pool's free map, trashfs's
 *  on-disk block bitmap, …). It does NOT own memory: you hand it a
 *  word array (`bitset_words(nbits)` of them) and it tracks bits in it.
 *  That lets the storage live wherever the owner needs — a malloc'd
 *  buffer, a static array, or embedded in an on-disk region.
 *
 *  CONVENTION for allocators: treat a SET bit as "slot in use" and a
 *  CLEAR bit as "slot free". `bitset_find_first_clear` is then the
 *  allocate primitive (grab a free slot), `bitset_set` marks it used,
 *  `bitset_clear` frees it, and `bitset_popcount` is the used count.
 *  (The set/clear ops are symmetric, so the opposite polarity works
 *  too — just read the verbs the other way.)
 *
 *  Scanning is word-at-a-time via math/bits.h (ctz): a full word is
 *  skipped in one compare, and the free bit inside the first non-full
 *  word is found with one ctz — so on the Cortex-M it's the CLZ/RBIT
 *  hardware path, not a bit-at-a-time loop. `find_first_clear` keeps a
 *  rolling hint so repeated allocation is near-O(1) amortized rather
 *  than rescanning from 0.
 *
 *  Storage layout is plain little-endian-equivalent words; on an LE
 *  target the byte image is identical to a byte-addressed bitmap
 *  (bit b at byte b/8), so it is layout-compatible with existing
 *  byte-based bitmaps on LE hosts/MCUs.
 *
 *  Thread safety: none. Single-context use.
 *
 *  Depends on: math/bits.h.
 *
 *  Public domain (CC0). No warranty.
 *  https://creativecommons.org/publicdomain/zero/1.0/
 * ============================================================ */

#ifndef GARBAGE_CONTAINERS_BITSET_H
#define GARBAGE_CONTAINERS_BITSET_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* "not found" sentinel for the find_* scans. */
#define BITSET_NPOS  ((size_t)-1)

/* Number of 32-bit words needed to hold `nbits` bits (compile-time). */
#define BITSET_WORDS(nbits)  (((size_t)(nbits) + 31u) / 32u)

/* Bytes of word storage for `nbits` bits (compile-time). */
#define BITSET_BYTES(nbits)  (BITSET_WORDS(nbits) * sizeof(uint32_t))

typedef struct {
    uint32_t *words;    /* caller-owned, >= BITSET_WORDS(nbits) entries */
    size_t    nbits;    /* logical bit count                           */
    size_t    nwords;   /* BITSET_WORDS(nbits)                         */
    size_t    hint;     /* rolling start word for find_first_clear     */
} Bitset;

/* Runtime word/byte sizing — match the macros. */
size_t bitset_words(size_t nbits);
size_t bitset_bytes(size_t nbits);

/* Bind to caller storage. Does NOT modify the words — call
 * bitset_clear_all/set_all to define the initial state. `words` must
 * have >= bitset_words(nbits) entries and outlive the bitset. */
void bitset_init(Bitset *bs, uint32_t *words, size_t nbits);

/* Set every bit to 0 (all free) / 1 (all used). Tail bits beyond
 * nbits in the last word are kept 0 so popcount/find stay correct. */
void bitset_clear_all(Bitset *bs);
void bitset_set_all(Bitset *bs);

/* Single-bit ops. Out-of-range indices are ignored (set/clear) or read
 * as 0 (test). */
void bitset_set(Bitset *bs, size_t i);
void bitset_clear(Bitset *bs, size_t i);
bool bitset_test(const Bitset *bs, size_t i);

/* Number of set bits (the "used" count for the allocator convention). */
size_t bitset_popcount(const Bitset *bs);

/* Lowest set bit index, or BITSET_NPOS if none. */
size_t bitset_find_first_set(const Bitset *bs);

/* Lowest CLEAR bit index (a free slot), or BITSET_NPOS if all set.
 * Uses + advances the rolling hint, so this is the cheap allocate
 * primitive under repeated calls. Does NOT modify the bit — set it
 * yourself once you've taken it. */
size_t bitset_find_first_clear(Bitset *bs);

#endif /* GARBAGE_CONTAINERS_BITSET_H */
