/* test_presched_spawn.c — dynamic (runtime) task creation in presched.
 *
 * Proves the primitive that preemptive SYS_SPAWN_AND_WAIT relies on: a
 * task, while the scheduler is already running, adds a NEW task
 * (presched_add_task) and blocks until it finishes; the new task runs,
 * does work, wakes the parent, and exits. Validates dynamic task
 * creation + scheduling + the join/running_count accounting + sticky
 * block/wake — the spawn pattern, in isolation, with no VM or FS.
 *
 * POSIX validates under TSan; Windows is user-verified (same boundary
 * as the rest of presched).
 *
 * Build (Windows/mingw):
 *   gcc -std=c11 -I include -o build/t_spawn.exe \
 *       src/vm/tests/test_presched_spawn.c src/vm/presched.c -lpthread
 *
 * Public domain (CC0). No warranty.
 */

#include "vm/presched.h"
#include <stdio.h>
#include <stdatomic.h>

static PreSched   *g_s;
static atomic_int  g_child_ran          = 0;
static atomic_int  g_child_done         = 0;
static atomic_int  g_parent_saw_child   = 0;
static atomic_int  g_spawn_id           = -2;

typedef struct { int parent_id; } ChildArg;
static ChildArg g_ca;

static void child_body(void *arg) {
    ChildArg *ca = (ChildArg *)arg;
    /* a little compute so it must be genuinely scheduled, not instant */
    volatile unsigned long x = 0;
    for (unsigned long i = 0; i < 5000000UL; i++) x += i;
    atomic_store(&g_child_ran, 1);
    atomic_store(&g_child_done, 1);
    presched_wake(g_s, ca->parent_id);   /* wake the spawn-waiting parent */
}

static void parent_body(void *arg) {
    (void)arg;
    g_ca.parent_id = presched_self_id();
    int cid = presched_add_task(g_s, child_body, &g_ca);  /* spawn at runtime */
    atomic_store(&g_spawn_id, cid);
    if (cid < 0) return;
    /* wait for the child (sticky block: a wake before we park isn't lost) */
    while (!atomic_load(&g_child_done))
        presched_block(g_s);
    if (atomic_load(&g_child_ran))
        atomic_store(&g_parent_saw_child, 1);
}

int main(void) {
    g_s = presched_create(1000);
    if (!g_s) { printf("create failed\n"); return 1; }
    presched_add_task(g_s, parent_body, NULL);   /* one task to start */
    presched_run(g_s);                            /* parent spawns the child */

    int spawn_id   = atomic_load(&g_spawn_id);
    int child_ran  = atomic_load(&g_child_ran);
    int parent_saw = atomic_load(&g_parent_saw_child);
    int ok = (spawn_id >= 0) && child_ran && parent_saw;
    printf("dynamic-spawn: spawn_id=%d child_ran=%d parent_resumed=%d switches=%llu -> %s\n",
           spawn_id, child_ran, parent_saw,
           (unsigned long long)presched_switches(g_s),
           ok ? "WORKS" : "FAILED");
    presched_destroy(g_s);
    return ok ? 0 : 1;
}
