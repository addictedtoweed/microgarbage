/* 07_vm_task/guest.c — a compute-bound guest that runs as a PREEMPTIVE
 * task. It does a deterministic amount of work and exits with a known
 * code, so the host can verify it ran correctly while being preempted by
 * the scheduler's systick and interleaved with a native task.
 *
 * No yields: unlike 02_counter (cooperative), this guest never yields —
 * it just computes. The preemptive scheduler interrupts it externally
 * via the systick. That is the whole point of Step 4: a VM task needs no
 * cooperation to share the CPU.
 *
 * Public domain (CC0). No warranty.
 */
#define SYS_EXIT 93

static inline void sys_exit(int code) {
    register int a0 asm("a0") = code;
    register int a7 asm("a7") = SYS_EXIT;
    asm volatile("ecall" :: "r"(a0), "r"(a7));
    __builtin_unreachable();
}

void _start(void) {
    volatile unsigned long sum = 0;
    for (unsigned i = 0; i < 2000000u; i++) sum += i;
    sys_exit((int)(sum & 0x7f));   /* deterministic: sum(0..1999999)&0x7f = 64 */
}
