/* ============================================================
 *  presched.c — preemptive thread-per-task scheduler (skeleton)
 *  Public domain (CC0). No warranty.
 *
 *  See presched.h and docs/execution-model.md §7. Step 1: bare
 *  round-robin preemption, single priority level, native tasks.
 *
 *  TRUE preemption (proven mechanism): a periodic systick interrupts
 *  the RUNNING task mid-body.
 *    - POSIX: a realtime timer signal is delivered to the process; the
 *      handler runs in whichever thread is executing. Only the task
 *      that currently holds the CPU reacts (my_id == current): it picks
 *      the next ready task, releases that task's gate, and parks itself
 *      on its own gate. So the running body is genuinely interrupted
 *      and a different task proceeds. Validated in-sandbox: tasks
 *      interleave with fair progress, no deadlock, TSan-clean.
 *    - Windows: a timer thread Suspend/Resume-s task threads directly
 *      (the real async stop). Compile-checked; user-verified.
 *
 *  Single core: exactly one task runs at a time, enforced by the gates.
 * ============================================================ */

/* Feature-test macro MUST precede all includes (for timer_t, sigaction,
 * sigevent on glibc). */
#if !(defined(_WIN32) && !defined(__CYGWIN__))
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 199309L
#  endif
#endif

#include "vm/presched.h"

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

typedef enum { TASK_READY, TASK_RUNNING, TASK_DONE } TaskState;

#if defined(_WIN32) && !defined(__CYGWIN__)
/* ===================== Native Windows ===================== */
#include <windows.h>

typedef struct {
    presched_task_fn fn;
    void            *arg;
    HANDLE           thread;
    HANDLE           gate;        /* auto-reset event */
    _Atomic int      state;
    PreSched        *sched;
    int              id;
} Task;

struct PreSched {
    Task     tasks[PRESCHED_MAX_TASKS];
    int      n_tasks;
    unsigned tick_us;
    _Atomic int      current;
    _Atomic int      running_count;
    _Atomic uint64_t ticks;
    _Atomic uint64_t switches;
    _Atomic bool     stop;
    HANDLE   systick_thread;
};

static int next_ready(PreSched *s, int from) {
    for (int i = 1; i <= s->n_tasks; i++) {
        int idx = (from + i) % s->n_tasks;
        int st = atomic_load(&s->tasks[idx].state);
        if (st == TASK_READY || st == TASK_RUNNING) return idx;
    }
    return -1;
}

static DWORD WINAPI systick_main(LPVOID arg) {
    PreSched *s = (PreSched *)arg;
    while (!atomic_load(&s->stop)) {
        Sleep(s->tick_us / 1000 ? s->tick_us / 1000 : 1);
        atomic_fetch_add(&s->ticks, 1);
        int cur = atomic_load(&s->current);
        int nxt = next_ready(s, cur < 0 ? 0 : cur);
        if (nxt < 0 || nxt == cur) continue;
        if (cur >= 0 && atomic_load(&s->tasks[cur].state) == TASK_RUNNING) {
            SuspendThread(s->tasks[cur].thread);    /* true async stop */
            atomic_store(&s->tasks[cur].state, TASK_READY);
        }
        atomic_store(&s->current, nxt);
        atomic_store(&s->tasks[nxt].state, TASK_RUNNING);
        atomic_fetch_add(&s->switches, 1);
        ResumeThread(s->tasks[nxt].thread);
    }
    return 0;
}

static DWORD WINAPI task_trampoline(LPVOID arg) {
    Task *t = (Task *)arg;
    PreSched *s = t->sched;
    WaitForSingleObject(t->gate, INFINITE);     /* wait for first grant */
    if (!atomic_load(&s->stop)) t->fn(t->arg);
    atomic_store(&t->state, TASK_DONE);
    /* hand off if I was current */
    if (atomic_load(&s->current) == t->id) {
        int nxt = next_ready(s, t->id);
        if (nxt >= 0 && nxt != t->id) {
            atomic_store(&s->current, nxt);
            atomic_store(&s->tasks[nxt].state, TASK_RUNNING);
            atomic_fetch_add(&s->switches, 1);
            ResumeThread(s->tasks[nxt].thread);
        }
    }
    if (atomic_fetch_sub(&s->running_count, 1) == 1)
        atomic_store(&s->stop, true);
    return 0;
}

PreSched *presched_create(unsigned tick_us) {
    PreSched *s = (PreSched *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->tick_us = tick_us ? tick_us : 1000;
    atomic_store(&s->current, -1);
    return s;
}
int presched_add_task(PreSched *s, presched_task_fn fn, void *arg) {
    if (!s || !fn || s->n_tasks >= PRESCHED_MAX_TASKS) return -1;
    int id = s->n_tasks++;
    Task *t = &s->tasks[id];
    t->fn = fn; t->arg = arg; t->sched = s; t->id = id;
    atomic_store(&t->state, TASK_READY);
    t->gate = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!t->gate) { s->n_tasks--; return -1; }
    return id;
}
void presched_run(PreSched *s) {
    if (!s || s->n_tasks == 0) return;
    atomic_store(&s->running_count, s->n_tasks);
    atomic_store(&s->stop, false);
    for (int i = 0; i < s->n_tasks; i++)
        s->tasks[i].thread = CreateThread(NULL, 0, task_trampoline, &s->tasks[i], 0, NULL);
    int first = next_ready(s, 0);
    if (first >= 0) {
        atomic_store(&s->current, first);
        atomic_store(&s->tasks[first].state, TASK_RUNNING);
        SetEvent(s->tasks[first].gate);
    }
    s->systick_thread = CreateThread(NULL, 0, systick_main, s, 0, NULL);
    for (int i = 0; i < s->n_tasks; i++) {
        WaitForSingleObject(s->tasks[i].thread, INFINITE);
        CloseHandle(s->tasks[i].thread);
    }
    atomic_store(&s->stop, true);
    WaitForSingleObject(s->systick_thread, INFINITE);
    CloseHandle(s->systick_thread);
}
void presched_destroy(PreSched *s) {
    if (!s) return;
    for (int i = 0; i < s->n_tasks; i++) if (s->tasks[i].gate) CloseHandle(s->tasks[i].gate);
    free(s);
}

#else
/* ===================== POSIX (validated) ===================== */
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <time.h>

typedef struct {
    presched_task_fn fn;
    void            *arg;
    pthread_t        thread;
    sem_t            gate;        /* run gate: posted = may run */
    _Atomic int      state;
    PreSched        *sched;
    int              id;
} Task;

struct PreSched {
    Task     tasks[PRESCHED_MAX_TASKS];
    int      n_tasks;
    unsigned tick_us;
    _Atomic int      current;       /* id holding the CPU, or -1 */
    _Atomic int      running_count;
    _Atomic uint64_t ticks;
    _Atomic uint64_t switches;
    _Atomic bool     stop;
    timer_t  timer;
    sem_t    done_sem;
};

static _Thread_local Task *tls_self = NULL;
static PreSched *g_active = NULL;    /* single active scheduler (skeleton) */

static int next_ready(PreSched *s, int from) {
    for (int i = 1; i <= s->n_tasks; i++) {
        int idx = (from + i) % s->n_tasks;
        int st = atomic_load(&s->tasks[idx].state);
        if (st == TASK_READY || st == TASK_RUNNING) return idx;
    }
    return -1;
}

/* Systick handler: runs in whichever thread is executing. Only the
 * task currently holding the CPU reacts — it rotates to the next ready
 * task, releases that task's gate, and parks itself on its own gate
 * (genuinely interrupting its body). sem_post/sem_wait here follow the
 * proven probe; sem_post is async-signal-safe, and parking via sem_wait
 * is how the running thread cedes the CPU mid-body. */
static void systick_handler(int sig) {
    (void)sig;
    PreSched *s = g_active;
    Task *self = tls_self;
    if (!s || !self) return;
    atomic_fetch_add(&s->ticks, 1);
    if (atomic_load(&s->current) != self->id) return;   /* not running */
    if (atomic_load(&s->stop)) return;

    int nxt = next_ready(s, self->id);
    if (nxt < 0 || nxt == self->id) return;             /* nobody else */

    atomic_store(&self->state, TASK_READY);
    atomic_store(&s->current, nxt);
    atomic_store(&s->tasks[nxt].state, TASK_RUNNING);
    atomic_fetch_add(&s->switches, 1);
    sem_post(&s->tasks[nxt].gate);      /* release the next task   */
    while (sem_wait(&self->gate) != 0)  /* park myself until re-run */
        ;
}

static void *task_trampoline(void *arg) {
    Task *t = (Task *)arg;
    PreSched *s = t->sched;
    tls_self = t;

    while (sem_wait(&t->gate) != 0) ;          /* wait for first grant */
    if (!atomic_load(&s->stop)) t->fn(t->arg); /* run body (preemptible) */
    atomic_store(&t->state, TASK_DONE);

    /* hand the CPU on if I held it */
    if (atomic_load(&s->current) == t->id) {
        int nxt = next_ready(s, t->id);
        if (nxt >= 0 && nxt != t->id) {
            atomic_store(&s->current, nxt);
            atomic_store(&s->tasks[nxt].state, TASK_RUNNING);
            atomic_fetch_add(&s->switches, 1);
            sem_post(&s->tasks[nxt].gate);
        }
    }
    if (atomic_fetch_sub(&s->running_count, 1) == 1) {
        atomic_store(&s->stop, true);
        sem_post(&s->done_sem);
    }
    return NULL;
}

PreSched *presched_create(unsigned tick_us) {
    PreSched *s = (PreSched *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->tick_us = tick_us ? tick_us : 1000;
    atomic_store(&s->current, -1);
    sem_init(&s->done_sem, 0, 0);
    return s;
}

int presched_add_task(PreSched *s, presched_task_fn fn, void *arg) {
    if (!s || !fn || s->n_tasks >= PRESCHED_MAX_TASKS) return -1;
    int id = s->n_tasks++;
    Task *t = &s->tasks[id];
    t->fn = fn; t->arg = arg; t->sched = s; t->id = id;
    atomic_store(&t->state, TASK_READY);
    if (sem_init(&t->gate, 0, 0) != 0) { s->n_tasks--; return -1; }
    return id;
}

void presched_run(PreSched *s) {
    if (!s || s->n_tasks == 0) return;
    atomic_store(&s->running_count, s->n_tasks);
    atomic_store(&s->stop, false);
    g_active = s;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = systick_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;   /* EINTR is handled by the sem_wait retry loops */
    sigaction(SIGRTMIN, &sa, NULL);

    for (int i = 0; i < s->n_tasks; i++)
        pthread_create(&s->tasks[i].thread, NULL, task_trampoline, &s->tasks[i]);

    /* periodic systick -> SIGRTMIN */
    struct sigevent sev;
    memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo  = SIGRTMIN;
    timer_create(CLOCK_MONOTONIC, &sev, &s->timer);
    struct itimerspec its;
    memset(&its, 0, sizeof its);
    its.it_value.tv_nsec    = (long)s->tick_us * 1000;
    its.it_interval.tv_nsec = (long)s->tick_us * 1000;
    while (its.it_value.tv_nsec >= 1000000000L) {
        its.it_value.tv_nsec    -= 1000000000L; its.it_value.tv_sec++;
        its.it_interval.tv_nsec -= 1000000000L; its.it_interval.tv_sec++;
    }
    timer_settime(s->timer, 0, &its, NULL);

    int first = next_ready(s, 0);
    if (first >= 0) {
        atomic_store(&s->current, first);
        atomic_store(&s->tasks[first].state, TASK_RUNNING);
        sem_post(&s->tasks[first].gate);
    }

    while (sem_wait(&s->done_sem) != 0) ;        /* all tasks finished */

    timer_delete(s->timer);
    for (int i = 0; i < s->n_tasks; i++) {
        sem_post(&s->tasks[i].gate);             /* unblock any parked */
        pthread_join(s->tasks[i].thread, NULL);
    }
    g_active = NULL;
}

void presched_destroy(PreSched *s) {
    if (!s) return;
    for (int i = 0; i < s->n_tasks; i++) sem_destroy(&s->tasks[i].gate);
    sem_destroy(&s->done_sem);
    free(s);
}

#endif

uint64_t presched_total_ticks(const PreSched *s) {
    return s ? atomic_load((_Atomic uint64_t *)&s->ticks) : 0;
}
uint64_t presched_switches(const PreSched *s) {
    return s ? atomic_load((_Atomic uint64_t *)&s->switches) : 0;
}
