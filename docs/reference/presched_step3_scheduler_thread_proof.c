/* ============================================================
 *  REFERENCE PROOF (not part of the built library)
 *
 *  Standalone demonstration of the Step 3 scheduler rework described in
 *  docs/scheduler-step3-rework.md. Proves the dedicated-scheduler-thread
 *  + sigwait + directed-pthread_kill model resolves the lone-sleeper
 *  deadlock: a single task that sleeps with no other ready task wakes at
 *  exactly its deadline.
 *
 *  Build & run:
 *      cc -std=c11 presched_step3_scheduler_thread_proof.c -lrt -lpthread -o proof
 *      ./proof      # expect: sleeper_done=1 ticks=50
 *
 *  This is the shape to rebuild presched.c on — see the rework doc.
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#define _GNU_SOURCE
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <stdatomic.h>

/* SIGUSR1 = "park yourself" sent DIRECTED to the running task thread.
 * A dedicated scheduler thread uses sigwait on SIGRTMIN (the tick),
 * does bookkeeping, and pthread_kills the running task to preempt it. */
enum{READY,RUNNING,BLOCKED,DONE};
#define N 2
#define IDLE 1
static sem_t gate[N];
static atomic_int state[N], prio[N], current=-1;
static atomic_ullong ticks=0, wake_deadline[N];
static atomic_int running_count=1, sleeper_done=0;
static pthread_t threads[N];
static _Thread_local int me=-1;

static int hi(void){int b=-1;for(int i=0;i<N;i++){int s=atomic_load(&state[i]);if((s==READY||s==RUNNING)&&prio[i]>b)b=prio[i];}return b;}
static int pick(int from){int l=hi();if(l<0)return -1;for(int k=1;k<=N;k++){int i=(from+k)%N;if(prio[i]!=l)continue;int s=atomic_load(&state[i]);if(s==READY||s==RUNNING)return i;}if(prio[from%N]==l){int s=atomic_load(&state[from%N]);if(s==READY||s==RUNNING)return from%N;}return -1;}

/* park signal handler: the running task, when told, parks on its gate */
static void on_park(int s){(void)s; /* just return; parking done by caller loop */ }

static void *sched_thread(void *a){(void)a;
  sigset_t set; sigemptyset(&set); sigaddset(&set,SIGRTMIN);
  while(atomic_load(&running_count)>0){
    int sig; sigwait(&set,&sig);                 /* consume one tick */
    unsigned long long now=atomic_fetch_add(&ticks,1)+1;
    for(int i=0;i<N;i++){unsigned long long dl=atomic_load(&wake_deadline[i]);
      if(dl&&now>=dl&&atomic_load(&state[i])==BLOCKED){atomic_store(&wake_deadline[i],0);atomic_store(&state[i],READY);}}
    int cur=atomic_load(&current); int nxt=pick(cur<0?0:cur);
    if(nxt>=0&&nxt!=cur){
      if(cur>=0&&atomic_load(&state[cur])==RUNNING){atomic_store(&state[cur],READY); /* ask cur to park */
        atomic_store(&current,-1); pthread_kill(threads[cur],SIGUSR1);}
      atomic_store(&current,nxt); atomic_store(&state[nxt],RUNNING); sem_post(&gate[nxt]);
    }
  }
  return NULL;
}
static void do_sleep(unsigned n){atomic_store(&wake_deadline[me],atomic_load(&ticks)+n);atomic_store(&state[me],BLOCKED);
  atomic_store(&current,-1); while(sem_wait(&gate[me])!=0);}
static void *sleeper(void*a){(void)a;me=0;while(sem_wait(&gate[me])!=0);
  do_sleep(50); sleeper_done=1; atomic_store(&state[me],DONE); atomic_fetch_sub(&running_count,1); atomic_store(&current,-1); return NULL;}
static void *idle(void*a){(void)a;me=IDLE;while(sem_wait(&gate[me])!=0);
  while(atomic_load(&running_count)>0){ if(atomic_load(&current)!=IDLE){while(sem_wait(&gate[me])!=0);} struct timespec t={0,50000};nanosleep(&t,NULL);} return NULL;}

int main(void){
  for(int i=0;i<N;i++){sem_init(&gate[i],0,0);atomic_store(&state[i],READY);wake_deadline[i]=0;}
  prio[0]=4;prio[IDLE]=0;
  sigset_t set;sigemptyset(&set);sigaddset(&set,SIGRTMIN);pthread_sigmask(SIG_BLOCK,&set,NULL); /* block tick in all threads; sched uses sigwait */
  struct sigaction sa;sa.sa_handler=on_park;sa.sa_flags=0;sigemptyset(&sa.sa_mask);sigaction(SIGUSR1,&sa,NULL);
  pthread_t sch; pthread_create(&sch,NULL,sched_thread,NULL);
  pthread_create(&threads[0],NULL,sleeper,NULL); pthread_create(&threads[1],NULL,idle,NULL);
  timer_t tid;struct sigevent sev;sev.sigev_notify=SIGEV_SIGNAL;sev.sigev_signo=SIGRTMIN;sev.sigev_value.sival_ptr=0;
  timer_create(CLOCK_MONOTONIC,&sev,&tid);struct itimerspec its={{0,1000000},{0,1000000}};timer_settime(tid,0,&its,NULL);
  int f=pick(0);atomic_store(&current,f);atomic_store(&state[f],RUNNING);sem_post(&gate[f]);
  pthread_join(threads[0],NULL); pthread_join(sch,NULL); sem_post(&gate[IDLE]); pthread_join(threads[1],NULL);
  timer_delete(tid);
  printf("sleeper_done=%d ticks=%llu\n",sleeper_done,(unsigned long long)ticks);
  return sleeper_done?0:1;
}
