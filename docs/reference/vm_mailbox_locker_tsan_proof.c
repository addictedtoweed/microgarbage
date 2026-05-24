/* ============================================================
 *  REFERENCE PROOF (not part of the built library or test suite)
 *
 *  Demonstrates that the mailbox locker seam (vm_mailbox_set_locker)
 *  serializes concurrent access, validating Step 5 build-order item #1
 *  (see docs/scheduler-step5-ecall-audit.md).
 *
 *  Two threads hammer one mailbox (a sender and a receiver, 20000 msgs).
 *
 *  Build & run:
 *    # with a real locker: clean under TSan
 *    cc -std=c11 -fsanitize=thread vm_mailbox_locker_tsan_proof.c \
 *       ../../src/vm/vm_mailbox.c ../../src/containers/fifo_queue.c \
 *       ../../src/containers/ring_buffer.c -I../../include -lpthread -o proof
 *    ./proof           # locker installed -> 0 races, OK
 *    ./proof nolock    # null locker -> TSan reports the race the audit predicted
 *
 *  Result: with the pthread-mutex locker, TSan reports no data races and
 *  counts are exact (20000/20000). With the null locker (the cooperative
 *  default), TSan reports data races — confirming the seam is load-bearing
 *  and that the cooperative single-handler model is what makes null safe
 *  there. Public domain (CC0). No warranty.
 * ============================================================ */
#include "vm/vm_mailbox.h"
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static uintptr_t mtx_lock(void *c){ (void)c; pthread_mutex_lock(&g_mtx); return 0; }
static void mtx_unlock(void *c, uintptr_t s){ (void)c; (void)s; pthread_mutex_unlock(&g_mtx); }

static VmMailbox g_m;
static atomic_int g_sent = 0, g_recvd = 0;
#define N 20000

static void *sender(void *a){ (void)a;
  for(int i=0;i<N;i++){ uint32_t payload = (uint32_t)i;
    while(vm_mailbox_send(&g_m, 1, &payload, sizeof payload) != VM_MBOX_OK){ /* full: retry */ }
    atomic_fetch_add(&g_sent,1); }
  return NULL; }
static void *receiver(void *a){ (void)a;
  int got=0; uint32_t payload; uint16_t snd;
  while(got<N){ if(vm_mailbox_recv(&g_m,&payload,&snd)==VM_MBOX_OK){ got++; atomic_fetch_add(&g_recvd,1);} }
  return NULL; }

int main(int argc, char**argv){
  int use_lock = !(argc>1 && strcmp(argv[1],"nolock")==0);
  static uint8_t storage[ (4+2) * 64 ];
  vm_mailbox_init(&g_m, storage, sizeof(uint32_t), 64);
  vm_mailbox_whitelist_set(&g_m, 1);
  if(use_lock){ VmMailboxLocker lk = { mtx_lock, mtx_unlock, NULL }; vm_mailbox_set_locker(&g_m, lk); }
  printf("locker: %s\n", use_lock?"pthread-mutex":"NULL (expect races under TSan)");
  pthread_t s,r; pthread_create(&s,NULL,sender,NULL); pthread_create(&r,NULL,receiver,NULL);
  pthread_join(s,NULL); pthread_join(r,NULL);
  printf("sent=%d recvd=%d (expect %d each)\n", atomic_load(&g_sent), atomic_load(&g_recvd), N);
  printf("stats: accepted=%u recvs=%u\n", g_m.sends_accepted, g_m.recvs);
  int ok = (atomic_load(&g_sent)==N && atomic_load(&g_recvd)==N && g_m.sends_accepted==(uint32_t)N && g_m.recvs==(uint32_t)N);
  printf("%s\n", ok?"OK":"MISMATCH");
  return ok?0:1;
}
