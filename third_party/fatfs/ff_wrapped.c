/* ============================================================
 *  third_party/fatfs/ff_wrapped.c
 *
 *  Thin wrapper around upstream ff.c that silences the
 *  -Woverflow warning on FatFs's `c = DDEM` line (ff.c
 *  line ~2778). That assignment writes 0xE5 into a signed
 *  TCHAR (char) which GCC flags as an out-of-range narrowing
 *  conversion. The behavior is well-defined and correct —
 *  FatFs uses the byte pattern, not the signed value — but
 *  GCC has no way to know that from the source alone.
 *
 *  We don't patch upstream ff.c. Per-instance suppression
 *  isolated to this one translation unit keeps our build
 *  warning-clean without hiding overflow warnings anywhere
 *  else.
 *
 *  Public domain (CC0). The wrapped file is licensed per the
 *  FatFs upstream terms (BSD-2-clause-style); see ff.c header.
 * ============================================================ */

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Woverflow"
#endif

#include "source/ff.c"

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif
