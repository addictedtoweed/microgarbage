/* ============================================================
 *  time.h — minimal time for microgarbage guest VMs
 *
 *  time() over SYS_REALTIME_NOW (returns -1 if host has no
 *  realtime source). clock() over SYS_TICKS_NOW (monotonic
 *  ms since host start; wraps at 49.7 days).
 *
 *  Public domain (CC0).
 * ============================================================ */

#ifndef MICROGARBAGE_GUEST_TIME_H
#define MICROGARBAGE_GUEST_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t time_t;
typedef int32_t clock_t;

#define CLOCKS_PER_SEC 1000

time_t  time  (time_t *out);
clock_t clock (void);

#ifdef __cplusplus
}
#endif

#endif /* MICROGARBAGE_GUEST_TIME_H */
