/* Tests for presched — the preemptive scheduler skeleton (step 1).
 *
 * Validates the bare round-robin preemption mechanism: tasks run in
 * their own threads, a systick preempts the running task mid-body, and
 * the CPU rotates round-robin. We check that (a) every task completes
 * all its work (no starvation/deadlock), (b) the systick fired and
 * switched (preemption actually happened), and (c) work interleaves
 * rather than running serially (true mid-body preemption).
 *
 * POSIX (Linux/Cygwin): the timer-signal mechanism is exercised for
 * real here. Native Windows: the SuspendThread path is compile-checked
 * but this test's timing assertions are validated on the user's
 * machine (same boundary as channel_win32 / waveout).
 *
 * Link: -lrt -lpthread on POSIX.
 */

#include "test_runner.h"
#include "vm/presched.h"

#include <stdio.h>
#include <stdatomic.h>

#define NTASKS  4
#define CHUNKS  5

static atomic_int g_progress[NTASKS];
static atomic_int g_order[256];
static atomic_int g_order_n;

static void reset_state(void) {
    for (int i = 0; i < NTASKS; i++) atomic_store(&g_progress[i], 0);
    atomic_store(&g_order_n, 0);
}

/* Each task does CHUNKS work-chunks; after each it records its id in a
 * shared order log. Interleaved ids => preemption rotated the CPU. */
static void worker(void *arg) {
    int id = (int)(long)arg;
    for (int c = 0; c < CHUNKS; c++) {
        volatile unsigned long x = 0;
        for (unsigned long i = 0; i < 20000000UL; i++) x += i;
        int n = atomic_fetch_add(&g_order_n, 1);
        if (n < 256) atomic_store(&g_order[n], id);
        atomic_fetch_add(&g_progress[id], 1);
    }
}

static void test_all_tasks_complete(void) {
    reset_state();
    PreSched *s = presched_create(1000);   /* 1ms systick */
    ASSERT_NOT_NULL(s);
    for (long i = 0; i < NTASKS; i++)
        ASSERT(presched_add_task(s, worker, (void *)i) == (int)i);
    presched_run(s);
    for (int i = 0; i < NTASKS; i++)
        ASSERT_EQ_INT(CHUNKS, atomic_load(&g_progress[i]));   /* none starved */
    presched_destroy(s);
}

static void test_preemption_actually_switches(void) {
    reset_state();
    PreSched *s = presched_create(1000);
    for (long i = 0; i < NTASKS; i++) presched_add_task(s, worker, (void *)i);
    presched_run(s);
    /* the systick fired and the scheduler made context switches */
    ASSERT(presched_total_ticks(s) > 0);
    ASSERT(presched_switches(s) > 0);
    presched_destroy(s);
}

static void test_work_interleaves(void) {
    reset_state();
    PreSched *s = presched_create(1000);
    for (long i = 0; i < NTASKS; i++) presched_add_task(s, worker, (void *)i);
    presched_run(s);
    /* Count how many times the running task id CHANGES between adjacent
     * log entries. Serial execution (0000011111...) would have only
     * NTASKS-1 changes; true preemption interleaves, giving many more. */
    int n = atomic_load(&g_order_n);
    int changes = 0;
    for (int i = 1; i < n && i < 256; i++)
        if (atomic_load(&g_order[i]) != atomic_load(&g_order[i - 1])) changes++;
    ASSERT(changes > NTASKS);   /* far more interleaving than serial */
    presched_destroy(s);
}

static void test_single_task_runs(void) {
    /* a lone task with no one to round-robin against still completes */
    reset_state();
    PreSched *s = presched_create(1000);
    presched_add_task(s, worker, (void *)0);
    presched_run(s);
    ASSERT_EQ_INT(CHUNKS, atomic_load(&g_progress[0]));
    presched_destroy(s);
}

static void test_repeated_runs_no_deadlock(void) {
    /* run the whole thing several times; a timing-dependent deadlock
     * would hang the suite (caught by the harness/timeout). */
    for (int rep = 0; rep < 5; rep++) {
        reset_state();
        PreSched *s = presched_create(500);   /* 0.5ms: more switches */
        for (long i = 0; i < NTASKS; i++) presched_add_task(s, worker, (void *)i);
        presched_run(s);
        for (int i = 0; i < NTASKS; i++)
            ASSERT_EQ_INT(CHUNKS, atomic_load(&g_progress[i]));
        presched_destroy(s);
    }
}

int main(void) {
    RUN(test_all_tasks_complete);
    RUN(test_preemption_actually_switches);
    RUN(test_work_interleaves);
    RUN(test_single_task_runs);
    RUN(test_repeated_runs_no_deadlock);
    return TEST_SUITE_RESULT();
}
