/* ============================================================
 *  math.h — aggregator for math modules
 *
 *  Include this to pull in fixed_point, fast_div, and bits. Each
 *  module is independently usable; this header just saves you the
 *  #includes.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef GARBAGE_MATH_H
#define GARBAGE_MATH_H

/* Pass through to the C standard <math.h> first: with -Iinclude the
 * preprocessor finds THIS file when code does `#include <math.h>` (gcc
 * searches user paths for <>-includes too), so the system header would
 * otherwise be shadowed. #include_next is gcc-specific but the project
 * is mingw/gcc only. */
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wpedantic"
#  include_next <math.h>
#  pragma GCC diagnostic pop
#endif

#include "math/fixed_point.h"
#include "math/fast_div.h"
#include "math/bits.h"

#endif /* GARBAGE_MATH_H */
