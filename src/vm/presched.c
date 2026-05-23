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
#  ifndef _GNU_SOURCE
#    define _GNU_SOURCE   /* sigwait, pthread_kill, timer_t, sigaction */
#  endif
#endif

#include "vm/presched.h"

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

typedef enum { TASK_READY, TASK_RUNNING, TASK_BLOCKED, TASK_DONE } TaskState;

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
    int              priority;    /* 0..PRESCHED_PRIO_MAX */
    _Atomic int      pending_wake;     /* sticky wake (no lost wakeup) */
    _Atomic uint64_t wake_deadline;    /* tick to wake a sleeper (0=none) */
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

static _Thread_local Task *win_tls_self = NULL;

/* Highest priority level that currently has a ready/running task, or
 * -1 if none. (Small task count: a direct scan; the bitmap+clz form is
 * the micro optimization, equivalent result.) */
static int highest_ready_level(PreSched *s) {
    int best = -1;
    for (int i = 0; i < s->n_tasks; i++) {
        int st = atomic_load(&s->tasks[i].state);
        if (st == TASK_READY || st == TASK_RUNNING)
            if (s->tasks[i].priority > best) best = s->tasks[i].priority;
    }
    return best;
}

/* Pick the next runnable task: the highest occupied priority level,
 * round-robin among tasks AT that level starting after `from`. Returns
 * a task id or -1. Strict priority: lower levels are never chosen while
 * a higher level has a ready task. */
static int next_ready(PreSched *s, int from) {
    int lvl = highest_ready_level(s);
    if (lvl < 0) return -1;
    for (int i = 1; i <= s->n_tasks; i++) {
        int idx = (from + i) % s->n_tasks;
        if (s->tasks[idx].priority != lvl) continue;
        int st = atomic_load(&s->tasks[idx].state);
        if (st == TASK_READY || st == TASK_RUNNING) return idx;
    }
    /* `from` itself might be the only one at this level */
    if (s->tasks[from % s->n_tasks].priority == lvl) {
        int st = atomic_load(&s->tasks[from % s->n_tasks].state);
        if (st == TASK_READY || st == TASK_RUNNING) return from % s->n_tasks;
    }
    return -1;
}

static DWORD WINAPI systick_main(LPVOID arg) {
    PreSched *s = (PreSched *)arg;
    while (!atomic_load(&s->stop)) {
        Sleep(s->tick_us / 1000 ? s->tick_us / 1000 : 1);
        uint64_t now = atomic_fetch_add(&s->ticks, 1) + 1;
        /* wake any sleeper whose deadline has passed */
        for (int i = 0; i < s->n_tasks; i++) {
            uint64_t dl = atomic_load(&s->tasks[i].wake_deadline);
            if (dl != 0 && now >= dl &&
                atomic_load(&s->tasks[i].state) == TASK_BLOCKED) {
                atomic_store(&s->tasks[i].wake_deadline, 0);
                atomic_store(&s->tasks[i].state, TASK_READY);
            }
        }
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
    win_tls_self = t;
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

/* ---- block / wake / sleep (Windows; compile-checked, user-verified) ----
 * The systick thread drives time-wakes and preemption, so a blocking task
 * sets its state and waits its gate; the systick thread re-dispatches.
 * (Mechanism mirrors the POSIX path; SuspendThread/ResumeThread is the
 * Windows preemption primitive.) */
int presched_self_id(void) { return win_tls_self ? win_tls_self->id : -1; }

void presched_block(PreSched *s) {
    Task *self = win_tls_self;
    if (!s || !self) return;
    int expect = 1;
    if (atomic_compare_exchange_strong(&self->pending_wake, &expect, 0)) return;
    atomic_store(&self->state, TASK_BLOCKED);
    atomic_store(&s->current, -1);
    WaitForSingleObject(self->gate, INFINITE);   /* parked; systick re-dispatches */
}
void presched_wake(PreSched *s, int id) {
    if (!s || id < 0 || id >= s->n_tasks) return;
    Task *t = &s->tasks[id];
    int expect = TASK_BLOCKED;
    if (atomic_compare_exchange_strong(&t->state, &expect, TASK_READY))
        atomic_store(&t->wake_deadline, 0);
    else
        atomic_store(&t->pending_wake, 1);
    SetEvent(t->gate);
}
void presched_sleep(PreSched *s, uint32_t ticks) {
    Task *self = win_tls_self;
    if (!s || !self || ticks == 0) return;
    atomic_store(&self->wake_deadline, atomic_load(&s->ticks) + ticks);
    atomic_store(&self->state, TASK_BLOCKED);
    atomic_store(&s->current, -1);
    WaitForSingleObject(self->gate, INFINITE);   /* systick wakes us */
}

PreSched *presched_create(unsigned tick_us) {
    PreSched *s = (PreSched *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->tick_us = tick_us ? tick_us : 1000;
    atomic_store(&s->current, -1);
    return s;
}
int presched_add_task_prio(PreSched *s, presched_task_fn fn, void *arg, int priority) {
    if (!s || !fn || s->n_tasks >= PRESCHED_MAX_TASKS) return -1;
    if (priority < PRESCHED_PRIO_MIN) priority = PRESCHED_PRIO_MIN;
    if (priority > PRESCHED_PRIO_MAX) priority = PRESCHED_PRIO_MAX;
    int id = s->n_tasks++;
    Task *t = &s->tasks[id];
    t->fn = fn; t->arg = arg; t->sched = s; t->id = id; t->priority = priority;
    atomic_store(&t->state, TASK_READY);
    t->gate = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!t->gate) { s->n_tasks--; return -1; }
    return id;
}
int presched_add_task(PreSched *s, presched_task_fn fn, void *arg) {
    return presched_add_task_prio(s, fn, arg, PRESCHED_PRIO_LEVELS / 2);
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
/* ===================== POSIX (validated) =====================
 *
 * Reworked mechanism (docs/scheduler-step3-rework.md): a DEDICATED
 * SCHEDULER THREAD owns the systick. SIGRTMIN is blocked in every thread
 * and the scheduler thread consumes ticks via sigwait — deterministic,
 * not delivered to an arbitrary thread (the old bug). The scheduler
 * thread does all time bookkeeping (tick count, sleep-deadline wakes) and
 * drives preemption: to preempt the running task it sends a DIRECTED
 * SIGUSR1 to that task's thread (whose handler parks it), then releases
 * the next task. Task threads never handle the tick.
 *
 * No idle task is needed: the scheduler thread is always alive to fire
 * time-based wakes, which is what the idle task existed for.
 */
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
    int              priority;    /* 0..PRESCHED_PRIO_MAX */
    _Atomic int      pending_wake;     /* sticky wake (no lost wakeup) */
    _Atomic uint64_t wake_deadline;    /* tick to wake a sleeper (0=none) */
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
    pthread_t sched_thread;
    sem_t    done_sem;
};

static _Thread_local Task *tls_self = NULL;
static PreSched *g_active = NULL;    /* single active scheduler */

static int highest_ready_level(PreSched *s) {
    int best = -1;
    for (int i = 0; i < s->n_tasks; i++) {
        int st = atomic_load(&s->tasks[i].state);
        if (st == TASK_READY || st == TASK_RUNNING)
            if (s->tasks[i].priority > best) best = s->tasks[i].priority;
    }
    return best;
}

static int next_ready(PreSched *s, int from) {
    int lvl = highest_ready_level(s);
    if (lvl < 0) return -1;
    for (int i = 1; i <= s->n_tasks; i++) {
        int idx = (from + i) % s->n_tasks;
        if (s->tasks[idx].priority != lvl) continue;
        int st = atomic_load(&s->tasks[idx].state);
        if (st == TASK_READY || st == TASK_RUNNING) return idx;
    }
    if (from >= 0 && s->tasks[from % s->n_tasks].priority == lvl) {
        int st = atomic_load(&s->tasks[from % s->n_tasks].state);
        if (st == TASK_READY || st == TASK_RUNNING) return from % s->n_tasks;
    }
    return -1;
}

/* SIGUSR1: directed to a task thread by the scheduler to park it. The
 * handler does nothing; the actual park happens because the scheduler
 * has cleared this task's RUNNING state and the task re-waits its gate
 * at the cooperative checkpoint. To preempt mid-body we park right here:
 * wait on our own gate until the scheduler grants us the CPU again. */
static void park_signal(int sig) {
    (void)sig;
    Task *self = tls_self;
    PreSched *s = g_active;
    if (!self || !s) return;
    if (atomic_load(&self->state) == TASK_DONE) return;
    /* park until the scheduler re-grants the CPU */
    while (sem_wait(&self->gate) != 0) ;
}

/* The dedicated scheduler thread: consumes one tick per sigwait, does
 * bookkeeping, and drives preemption. */
static void *sched_main(void *arg) {
    PreSched *s = (PreSched *)arg;
    sigset_t set; sigemptyset(&set); sigaddset(&set, SIGRTMIN);
    while (atomic_load(&s->running_count) > 0) {
        int sig;
        if (sigwait(&set, &sig) != 0) continue;
        uint64_t now = atomic_fetch_add(&s->ticks, 1) + 1;
        if (atomic_load(&s->running_count) <= 0) break;

        /* wake any sleeper whose deadline passed */
        for (int i = 0; i < s->n_tasks; i++) {
            uint64_t dl = atomic_load(&s->tasks[i].wake_deadline);
            if (dl != 0 && now >= dl &&
                atomic_load(&s->tasks[i].state) == TASK_BLOCKED) {
                atomic_store(&s->tasks[i].wake_deadline, 0);
                atomic_store(&s->tasks[i].state, TASK_READY);
            }
        }

        int cur = atomic_load(&s->current);
        int nxt = next_ready(s, cur < 0 ? 0 : cur);
        if (nxt < 0) continue;            /* nobody runnable right now */
        if (nxt == cur) continue;         /* keep running the same one */

        /* preempt current (if any) by directing SIGUSR1 to park it */
        if (cur >= 0 && atomic_load(&s->tasks[cur].state) == TASK_RUNNING) {
            atomic_store(&s->tasks[cur].state, TASK_READY);
            atomic_store(&s->current, -1);
            pthread_kill(s->tasks[cur].thread, SIGUSR1);
        }
        atomic_store(&s->current, nxt);
        atomic_store(&s->tasks[nxt].state, TASK_RUNNING);
        atomic_fetch_add(&s->switches, 1);
        sem_post(&s->tasks[nxt].gate);
    }
    return NULL;
}

/* Hand the CPU to the next ready task (caller already set its own state). */
static void hand_off_from(PreSched *s, Task *self) {
    int nxt = next_ready(s, self->id);
    if (nxt >= 0 && nxt != self->id) {
        atomic_store(&s->current, nxt);
        atomic_store(&s->tasks[nxt].state, TASK_RUNNING);
        atomic_fetch_add(&s->switches, 1);
        sem_post(&s->tasks[nxt].gate);
    } else {
        atomic_store(&s->current, -1);   /* nobody ready; scheduler will pick on a tick */
    }
}

static void *task_trampoline(void *arg) {
    Task *t = (Task *)arg;
    PreSched *s = t->sched;
    tls_self = t;

    while (sem_wait(&t->gate) != 0) ;          /* wait for first grant */
    if (!atomic_load(&s->stop)) t->fn(t->arg); /* run body (preemptible) */
    atomic_store(&t->state, TASK_DONE);

    /* hand the CPU on if I held it */
    if (atomic_load(&s->current) == t->id)
        hand_off_from(s, t);

    if (atomic_fetch_sub(&s->running_count, 1) == 1) {
        atomic_store(&s->stop, true);
        sem_post(&s->done_sem);
    }
    return NULL;
}

/* ---- block / wake / sleep ---- */

int presched_self_id(void) { return tls_self ? tls_self->id : -1; }

void presched_block(PreSched *s) {
    Task *self = tls_self;
    if (!s || !self) return;
    int expect = 1;
    if (atomic_compare_exchange_strong(&self->pending_wake, &expect, 0))
        return;                              /* sticky wake consumed */
    atomic_store(&self->state, TASK_BLOCKED);
    hand_off_from(s, self);
    while (sem_wait(&self->gate) != 0) ;     /* parked until woken+granted */
}

void presched_wake(PreSched *s, int id) {
    if (!s || id < 0 || id >= s->n_tasks) return;
    Task *t = &s->tasks[id];
    int expect = TASK_BLOCKED;
    if (atomic_compare_exchange_strong(&t->state, &expect, TASK_READY))
        atomic_store(&t->wake_deadline, 0);
    else
        atomic_store(&t->pending_wake, 1);   /* sticky */
}

void presched_sleep(PreSched *s, uint32_t ticks) {
    Task *self = tls_self;
    if (!s || !self || ticks == 0) return;
    atomic_store(&self->wake_deadline, atomic_load(&s->ticks) + ticks);
    atomic_store(&self->state, TASK_BLOCKED);
    hand_off_from(s, self);
    while (sem_wait(&self->gate) != 0) ;     /* scheduler re-readies us */
}

PreSched *presched_create(unsigned tick_us) {
    PreSched *s = (PreSched *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->tick_us = tick_us ? tick_us : 1000;
    atomic_store(&s->current, -1);
    sem_init(&s->done_sem, 0, 0);
    return s;
}

int presched_add_task_prio(PreSched *s, presched_task_fn fn, void *arg, int priority) {
    if (!s || !fn || s->n_tasks >= PRESCHED_MAX_TASKS) return -1;
    if (priority < PRESCHED_PRIO_MIN) priority = PRESCHED_PRIO_MIN;
    if (priority > PRESCHED_PRIO_MAX) priority = PRESCHED_PRIO_MAX;
    int id = s->n_tasks++;
    Task *t = &s->tasks[id];
    t->fn = fn; t->arg = arg; t->sched = s; t->id = id; t->priority = priority;
    atomic_store(&t->state, TASK_READY);
    if (sem_init(&t->gate, 0, 0) != 0) { s->n_tasks--; return -1; }
    return id;
}
int presched_add_task(PreSched *s, presched_task_fn fn, void *arg) {
    return presched_add_task_prio(s, fn, arg, PRESCHED_PRIO_LEVELS / 2);
}

void presched_run(PreSched *s) {
    if (!s || s->n_tasks == 0) return;
    atomic_store(&s->running_count, s->n_tasks);
    atomic_store(&s->stop, false);
    g_active = s;

    /* Block SIGRTMIN in this (and inherited) threads so ONLY the
     * scheduler thread's sigwait receives the tick. Install SIGUSR1 as
     * the directed park signal. */
    sigset_t tickset; sigemptyset(&tickset); sigaddset(&tickset, SIGRTMIN);
    pthread_sigmask(SIG_BLOCK, &tickset, NULL);

    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = park_signal; sigemptyset(&sa.sa_mask); sa.sa_flags = 0;
    sigaction(SIGUSR1, &sa, NULL);

    /* scheduler thread (inherits the SIGRTMIN block; uses sigwait) */
    pthread_create(&s->sched_thread, NULL, sched_main, s);

    /* task threads (inherit the SIGRTMIN block too — good; they must not
     * receive the tick. They DO receive directed SIGUSR1.) */
    for (int i = 0; i < s->n_tasks; i++)
        pthread_create(&s->tasks[i].thread, NULL, task_trampoline, &s->tasks[i]);

    /* periodic systick -> SIGRTMIN (consumed by sched_thread's sigwait) */
    struct sigevent sev; memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_SIGNAL; sev.sigev_signo = SIGRTMIN;
    timer_create(CLOCK_MONOTONIC, &sev, &s->timer);
    struct itimerspec its; memset(&its, 0, sizeof its);
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
    /* The scheduler thread may be parked in sigwait. running_count is now
     * 0, so nudge it with one more SIGRTMIN so sigwait returns and its
     * loop condition exits. */
    pthread_kill(s->sched_thread, SIGRTMIN);
    pthread_join(s->sched_thread, NULL);
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
