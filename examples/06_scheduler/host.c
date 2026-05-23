/* 06_scheduler/host.c — a runnable demo of the preemptive scheduler.
 *
 * Unlike examples 01-05 (which run guest VM code), this demonstrates the
 * NATIVE preemptive scheduler directly: several native tasks, time-sliced
 * by a systick, at different priorities. It shows two things the
 * committed scheduler (Steps 1-2, see docs/execution-model.md §7) does:
 *
 *   1. PREEMPTION + round-robin among equal-priority tasks — they
 *      interleave their progress rather than running one-at-a-time to
 *      completion.
 *   2. STRICT PRIORITY — a higher-priority task runs to completion before
 *      any lower-priority task makes progress (and the scheduler will
 *      faithfully starve the low one until the high one is done; avoiding
 *      that is the task designer's job — here the high task finishes).
 *
 * This is the host (POSIX) backend. The same scheduler core is intended
 * to run on the MCU via the hooks in docs/scheduler-mcu-port.md (not
 * built here). Block/wake/sleep (Step 3) is not in this demo — it awaits
 * the scheduler-thread rework (docs/scheduler-step3-rework.md).
 *
 * Build:  ./build.sh        (or ./build.sh run)
 * Link:   -lrt -lpthread on POSIX.
 */

#include "vm/presched.h"

#include <stdio.h>
#include <stdatomic.h>

/* Each task does a few chunks of busy work, announcing each chunk so you
 * can see the interleaving in the output order. */
#define CHUNKS 5

static atomic_int g_log[256];
static atomic_int g_log_n;

static void record(int id) {
    int n = atomic_fetch_add(&g_log_n, 1);
    if (n < 256) atomic_store(&g_log[n], id);
}

static void burn(unsigned long iters) {
    volatile unsigned long x = 0;
    for (unsigned long i = 0; i < iters; i++) x += i;
}

/* a labeled worker: does CHUNKS chunks, recording its label each chunk */
static void worker(void *arg) {
    int id = (int)(long)arg;
    for (int c = 0; c < CHUNKS; c++) {
        burn(20000000UL);
        record(id);
        printf("  task %d: finished chunk %d/%d\n", id, c + 1, CHUNKS);
        fflush(stdout);
    }
}

static void print_order(void) {
    int n = atomic_load(&g_log_n);
    printf("  execution order (by task id): ");
    for (int i = 0; i < n && i < 256; i++) printf("%d", atomic_load(&g_log[i]));
    printf("\n");
}

static void demo_round_robin(void) {
    printf("\n=== Demo 1: preemptive round-robin (4 equal-priority tasks) ===\n");
    printf("Expect: interleaved progress (e.g. 0123012301...), not 0000011111...\n");
    atomic_store(&g_log_n, 0);
    PreSched *s = presched_create(1000);   /* 1 ms systick */
    for (long i = 0; i < 4; i++) presched_add_task(s, worker, (void *)i);
    presched_run(s);
    print_order();
    printf("  -> %llu systicks, %llu context switches\n",
           (unsigned long long)presched_total_ticks(s),
           (unsigned long long)presched_switches(s));
    presched_destroy(s);
}

static void demo_priority(void) {
    printf("\n=== Demo 2: strict priority (high vs low) ===\n");
    printf("Expect: task H (high) finishes all chunks before task L (low) starts.\n");
    atomic_store(&g_log_n, 0);
    PreSched *s = presched_create(1000);
    /* label 9 = high priority task, label 1 = low priority task */
    presched_add_task_prio(s, worker, (void *)9, PRESCHED_PRIO_MAX);
    presched_add_task_prio(s, worker, (void *)1, PRESCHED_PRIO_MIN);
    presched_run(s);
    print_order();
    printf("  -> note all 9s appear before any 1s: strict priority.\n");
    presched_destroy(s);
}

int main(void) {
    printf("microgarbage — preemptive scheduler demo (native tasks)\n");
    printf("(host/POSIX backend; same core targets the MCU via the port hooks)\n");
    demo_round_robin();
    demo_priority();
    printf("\nDone.\n");
    return 0;
}
