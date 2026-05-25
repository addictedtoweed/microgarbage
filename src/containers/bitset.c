/* ============================================================
 *  bitset.c — fixed-size bit set over caller word storage. See bitset.h.
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "containers/bitset.h"
#include "math/bits.h"

#define WORD_BITS  32u

/* Mask of valid bits in the last word (all-ones if nbits is a multiple
 * of 32). Used to ignore tail bits beyond nbits in set_all/scans. */
static uint32_t tail_mask(const Bitset *bs) {
    unsigned rem = (unsigned)(bs->nbits & 31u);
    return rem ? ((1u << rem) - 1u) : 0xFFFFFFFFu;
}

size_t bitset_words(size_t nbits) { return BITSET_WORDS(nbits); }
size_t bitset_bytes(size_t nbits) { return BITSET_BYTES(nbits); }

void bitset_init(Bitset *bs, uint32_t *words, size_t nbits) {
    if (!bs) return;
    bs->words  = words;
    bs->nbits  = nbits;
    bs->nwords = BITSET_WORDS(nbits);
    bs->hint   = 0;
}

void bitset_clear_all(Bitset *bs) {
    if (!bs || !bs->words) return;
    for (size_t w = 0; w < bs->nwords; w++) bs->words[w] = 0;
    bs->hint = 0;
}

void bitset_set_all(Bitset *bs) {
    if (!bs || !bs->words || bs->nwords == 0) return;
    for (size_t w = 0; w < bs->nwords; w++) bs->words[w] = 0xFFFFFFFFu;
    /* keep tail bits beyond nbits as 0 */
    bs->words[bs->nwords - 1] &= tail_mask(bs);
    bs->hint = 0;
}

void bitset_set(Bitset *bs, size_t i) {
    if (!bs || i >= bs->nbits) return;
    bs->words[i / WORD_BITS] |= (1u << (i & 31u));
}

void bitset_clear(Bitset *bs, size_t i) {
    if (!bs || i >= bs->nbits) return;
    bs->words[i / WORD_BITS] &= ~(1u << (i & 31u));
    /* a freed slot may sit before the hint; rewind so the next
     * allocate can find it without a full wrap. */
    size_t w = i / WORD_BITS;
    if (w < bs->hint) bs->hint = w;
}

bool bitset_test(const Bitset *bs, size_t i) {
    if (!bs || i >= bs->nbits) return false;
    return (bs->words[i / WORD_BITS] >> (i & 31u)) & 1u;
}

size_t bitset_popcount(const Bitset *bs) {
    if (!bs || !bs->words) return 0;
    size_t n = 0;
    for (size_t w = 0; w < bs->nwords; w++) n += bits_popcount32(bs->words[w]);
    return n;
}

size_t bitset_find_first_set(const Bitset *bs) {
    if (!bs || !bs->words) return BITSET_NPOS;
    for (size_t w = 0; w < bs->nwords; w++) {
        if (bs->words[w] != 0) {
            size_t i = w * WORD_BITS + bits_ctz32(bs->words[w]);
            return (i < bs->nbits) ? i : BITSET_NPOS;
        }
    }
    return BITSET_NPOS;
}

size_t bitset_find_first_clear(Bitset *bs) {
    if (!bs || !bs->words || bs->nwords == 0) return BITSET_NPOS;
    /* Scan from the rolling hint, then wrap to cover [0, hint). A clear
     * bit is a 0; ctz of the inverted word finds it in one step. The
     * last word's tail bits beyond nbits read as 0 in ~word, so OR in
     * the tail-invalid mask to skip them. */
    for (size_t pass = 0; pass < 2; pass++) {
        size_t start = (pass == 0) ? bs->hint : 0;
        size_t end   = (pass == 0) ? bs->nwords : bs->hint;
        for (size_t w = start; w < end; w++) {
            uint32_t inv = ~bs->words[w];
            if (w == bs->nwords - 1) inv &= tail_mask(bs);  /* ignore tail */
            if (inv != 0) {
                size_t i = w * WORD_BITS + bits_ctz32(inv);
                if (i < bs->nbits) { bs->hint = w; return i; }
            }
        }
    }
    return BITSET_NPOS;   /* full */
}
