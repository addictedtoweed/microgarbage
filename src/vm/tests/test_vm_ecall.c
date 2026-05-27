/* Tests for vm_ecall router.
 *
 * Uses tiny mock handlers (record-call, write-fixed-value) to
 * verify routing, registration, and fallback behavior without
 * needing the real handlers.
 * Public domain (CC0). No warranty. */

#include "test_runner.h"
#include "vm/vm_ecall.h"
#include "vm/vm_core.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* ============================================================
 *  Mock handlers
 *
 *  Each mock stuffs a recognizable value into a0 so the test
 *  can identify which handler ran. The 'system' pointer is
 *  used as a counter by some mocks so we can verify it's
 *  threaded through correctly.
 * ============================================================ */

static void mock_handler_a(VmCpu *cpu, void *system) {
    (void)system;
    cpu->regs[VM_REG_A0] = 0xAAAA;
}

static void mock_handler_b(VmCpu *cpu, void *system) {
    (void)system;
    cpu->regs[VM_REG_A0] = 0xBBBB;
}

static void mock_echo_a7(VmCpu *cpu, void *system) {
    (void)system;
    /* Echo the syscall number to a0 — lets us verify which
     * number triggered this handler. */
    cpu->regs[VM_REG_A0] = cpu->regs[VM_REG_A7];
}

static void mock_uses_system(VmCpu *cpu, void *system) {
    /* Treats the system pointer as a uint32_t* and writes it
     * to a0. The test can check that the pointer arrives
     * unmangled. */
    uint32_t *p = (uint32_t *)system;
    cpu->regs[VM_REG_A0] = *p;
}

static void mock_custom_fallback(VmCpu *cpu, void *system) {
    (void)system;
    cpu->regs[VM_REG_A0] = 0xDEAD;
}

/* ============================================================
 *  Basic init
 * ============================================================ */

static void test_init_clears_slots(void) {
    VmEcallRouter r;
    /* Pre-fill with garbage to ensure init clears */
    memset(&r, 0xAA, sizeof(r));
    vm_ecall_router_init(&r);

    /* All slots should be NULL after init. Use direct equality
     * rather than ASSERT_NULL/ASSERT_NOT_NULL because those cast
     * to void*, which is undefined for function pointers under
     * strict ISO C. */
    for (size_t i = 0; i < VM_ECALL_LINUX_RANGE_SIZE; i++) {
        ASSERT(r.linux_slots[i] == NULL);
    }
    for (size_t i = 0; i < VM_ECALL_VM_RANGE_SIZE; i++) {
        ASSERT(r.vm_slots[i] == NULL);
    }
    /* Fallback is set to the default (non-NULL) */
    ASSERT(r.fallback != NULL);
}

static void test_init_null_safe(void) {
    /* Shouldn't crash */
    vm_ecall_router_init(NULL);
}

/* ============================================================
 *  Registration
 * ============================================================ */

static void test_register_in_linux_range(void) {
    VmEcallRouter r;
    vm_ecall_router_init(&r);
    ASSERT(vm_ecall_register(&r, 93, mock_handler_a));
    ASSERT(r.linux_slots[93] == mock_handler_a);
}

static void test_register_in_vm_range(void) {
    VmEcallRouter r;
    vm_ecall_router_init(&r);
    ASSERT(vm_ecall_register(&r, 1024, mock_handler_b));
    ASSERT(r.vm_slots[0] == mock_handler_b);
}

static void test_register_out_of_range_fails(void) {
    VmEcallRouter r;
    vm_ecall_router_init(&r);
    /* 256..1023 is the gap between Linux and VM ranges */
    ASSERT(!vm_ecall_register(&r, 500, mock_handler_a));
    /* Above the VM range */
    ASSERT(!vm_ecall_register(&r, 2000, mock_handler_a));
}

static void test_register_conflict_fails(void) {
    VmEcallRouter r;
    vm_ecall_router_init(&r);
    ASSERT(vm_ecall_register(&r, 93, mock_handler_a));
    /* Second register should fail without overwriting */
    ASSERT(!vm_ecall_register(&r, 93, mock_handler_b));
    ASSERT(r.linux_slots[93] == mock_handler_a);
}

static void test_register_null_handler_fails(void) {
    VmEcallRouter r;
    vm_ecall_router_init(&r);
    ASSERT(!vm_ecall_register(&r, 93, NULL));
}

static void test_register_null_router_fails(void) {
    ASSERT(!vm_ecall_register(NULL, 93, mock_handler_a));
}

/* ============================================================
 *  Unregister
 * ============================================================ */

static void test_unregister(void) {
    VmEcallRouter r;
    vm_ecall_router_init(&r);
    ASSERT(vm_ecall_register(&r, 93, mock_handler_a));
    ASSERT(vm_ecall_unregister(&r, 93));
    ASSERT(r.linux_slots[93] == NULL);
}

static void test_unregister_unregistered_fails(void) {
    VmEcallRouter r;
    vm_ecall_router_init(&r);
    ASSERT(!vm_ecall_unregister(&r, 93));
}

static void test_unregister_out_of_range_fails(void) {
    VmEcallRouter r;
    vm_ecall_router_init(&r);
    ASSERT(!vm_ecall_unregister(&r, 500));
}

static void test_re_register_after_unregister(void) {
    VmEcallRouter r;
    vm_ecall_router_init(&r);
    ASSERT(vm_ecall_register(&r, 93, mock_handler_a));
    ASSERT(vm_ecall_unregister(&r, 93));
    /* Now should be able to register again */
    ASSERT(vm_ecall_register(&r, 93, mock_handler_b));
    ASSERT(r.linux_slots[93] == mock_handler_b);
}

/* ============================================================
 *  Dispatch
 * ============================================================ */

static void test_dispatch_calls_registered_handler(void) {
    VmEcallRouter r;
    vm_ecall_router_init(&r);
    vm_ecall_register(&r, 93, mock_handler_a);

    VmCpu cpu;
    vm_init(&cpu, 0);
    cpu.regs[VM_REG_A7] = 93;

    vm_ecall_dispatch(&r, &cpu, NULL);
    ASSERT_EQ_INT(0xAAAA, (int)cpu.regs[VM_REG_A0]);
}

static void test_dispatch_correct_handler_for_number(void) {
    /* Register different handlers at different numbers, verify
     * each one is called by the corresponding a7. */
    VmEcallRouter r;
    vm_ecall_router_init(&r);
    vm_ecall_register(&r, 93, mock_handler_a);
    vm_ecall_register(&r, 1024, mock_handler_b);

    VmCpu cpu;
    vm_init(&cpu, 0);

    cpu.regs[VM_REG_A7] = 93;
    vm_ecall_dispatch(&r, &cpu, NULL);
    ASSERT_EQ_INT(0xAAAA, (int)cpu.regs[VM_REG_A0]);

    cpu.regs[VM_REG_A7] = 1024;
    vm_ecall_dispatch(&r, &cpu, NULL);
    ASSERT_EQ_INT(0xBBBB, (int)cpu.regs[VM_REG_A0]);
}

static void test_dispatch_unregistered_calls_default_fallback(void) {
    /* Default fallback writes -ENOSYS to a0 */
    VmEcallRouter r;
    vm_ecall_router_init(&r);

    VmCpu cpu;
    vm_init(&cpu, 0);
    cpu.regs[VM_REG_A7] = 93;
    /* Pre-fill a0 so we can see it changed */
    cpu.regs[VM_REG_A0] = 0xCCCC;

    vm_ecall_dispatch(&r, &cpu, NULL);
    ASSERT_EQ_INT((int)(uint32_t)-VM_ENOSYS, (int)cpu.regs[VM_REG_A0]);
}

static void test_dispatch_out_of_range_calls_fallback(void) {
    VmEcallRouter r;
    vm_ecall_router_init(&r);

    VmCpu cpu;
    vm_init(&cpu, 0);
    cpu.regs[VM_REG_A7] = 500;   /* in the gap */
    cpu.regs[VM_REG_A0] = 0;

    vm_ecall_dispatch(&r, &cpu, NULL);
    ASSERT_EQ_INT((int)(uint32_t)-VM_ENOSYS, (int)cpu.regs[VM_REG_A0]);
}

static void test_dispatch_passes_a7_to_handler(void) {
    VmEcallRouter r;
    vm_ecall_router_init(&r);
    vm_ecall_register(&r, 1024, mock_echo_a7);

    VmCpu cpu;
    vm_init(&cpu, 0);
    cpu.regs[VM_REG_A7] = 1024;

    vm_ecall_dispatch(&r, &cpu, NULL);
    ASSERT_EQ_INT(1024, (int)cpu.regs[VM_REG_A0]);
}

static void test_dispatch_passes_system_pointer(void) {
    VmEcallRouter r;
    vm_ecall_router_init(&r);
    vm_ecall_register(&r, 1024, mock_uses_system);

    VmCpu cpu;
    vm_init(&cpu, 0);
    cpu.regs[VM_REG_A7] = 1024;

    uint32_t sys_value = 0xCAFEBABE;
    vm_ecall_dispatch(&r, &cpu, &sys_value);
    ASSERT_EQ_INT((int)0xCAFEBABE, (int)cpu.regs[VM_REG_A0]);
}

/* ============================================================
 *  Custom fallback
 * ============================================================ */

static void test_set_custom_fallback(void) {
    VmEcallRouter r;
    vm_ecall_router_init(&r);
    vm_ecall_set_fallback(&r, mock_custom_fallback);

    VmCpu cpu;
    vm_init(&cpu, 0);
    cpu.regs[VM_REG_A7] = 93;   /* no handler registered */

    vm_ecall_dispatch(&r, &cpu, NULL);
    ASSERT_EQ_INT(0xDEAD, (int)cpu.regs[VM_REG_A0]);
}

static void test_set_null_fallback_restores_default(void) {
    VmEcallRouter r;
    vm_ecall_router_init(&r);
    vm_ecall_set_fallback(&r, mock_custom_fallback);
    /* Now reset back to default by passing NULL */
    vm_ecall_set_fallback(&r, NULL);

    VmCpu cpu;
    vm_init(&cpu, 0);
    cpu.regs[VM_REG_A7] = 93;

    vm_ecall_dispatch(&r, &cpu, NULL);
    ASSERT_EQ_INT((int)(uint32_t)-VM_ENOSYS, (int)cpu.regs[VM_REG_A0]);
}

static void test_registered_handler_beats_fallback(void) {
    /* If a number IS registered, the handler wins; fallback
     * is not consulted even if set. */
    VmEcallRouter r;
    vm_ecall_router_init(&r);
    vm_ecall_register(&r, 93, mock_handler_a);
    vm_ecall_set_fallback(&r, mock_custom_fallback);

    VmCpu cpu;
    vm_init(&cpu, 0);
    cpu.regs[VM_REG_A7] = 93;

    vm_ecall_dispatch(&r, &cpu, NULL);
    ASSERT_EQ_INT(0xAAAA, (int)cpu.regs[VM_REG_A0]);
}

/* ============================================================
 *  Boundary numbers
 * ============================================================ */

static void test_register_at_linux_range_max(void) {
    /* 255 should be valid; 256 should not. */
    VmEcallRouter r;
    vm_ecall_router_init(&r);
    ASSERT(vm_ecall_register(&r, 255, mock_handler_a));
    ASSERT(!vm_ecall_register(&r, 256, mock_handler_a));
}

static void test_register_at_vm_range_boundaries(void) {
    /* 1023 invalid, 1024 valid, 1279 valid, 1280 invalid */
    VmEcallRouter r;
    vm_ecall_router_init(&r);
    ASSERT(!vm_ecall_register(&r, 1023, mock_handler_a));
    ASSERT(vm_ecall_register(&r, 1024, mock_handler_a));
    ASSERT(vm_ecall_register(&r, 1279, mock_handler_b));
    ASSERT(!vm_ecall_register(&r, 1280, mock_handler_a));
}

/* ============================================================
 *  Null/edge case dispatch
 * ============================================================ */

static void test_dispatch_null_safe(void) {
    /* Shouldn't crash */
    vm_ecall_dispatch(NULL, NULL, NULL);

    VmEcallRouter r;
    vm_ecall_router_init(&r);
    vm_ecall_dispatch(&r, NULL, NULL);

    VmCpu cpu;
    vm_init(&cpu, 0);
    vm_ecall_dispatch(NULL, &cpu, NULL);
}

/* ============================================================
 *  Test runner
 * ============================================================ */

int main(void) {
    TEST_SUITE("vm_ecall");

    /* Init */
    RUN(test_init_clears_slots);
    RUN(test_init_null_safe);

    /* Register */
    RUN(test_register_in_linux_range);
    RUN(test_register_in_vm_range);
    RUN(test_register_out_of_range_fails);
    RUN(test_register_conflict_fails);
    RUN(test_register_null_handler_fails);
    RUN(test_register_null_router_fails);

    /* Unregister */
    RUN(test_unregister);
    RUN(test_unregister_unregistered_fails);
    RUN(test_unregister_out_of_range_fails);
    RUN(test_re_register_after_unregister);

    /* Dispatch */
    RUN(test_dispatch_calls_registered_handler);
    RUN(test_dispatch_correct_handler_for_number);
    RUN(test_dispatch_unregistered_calls_default_fallback);
    RUN(test_dispatch_out_of_range_calls_fallback);
    RUN(test_dispatch_passes_a7_to_handler);
    RUN(test_dispatch_passes_system_pointer);

    /* Fallback */
    RUN(test_set_custom_fallback);
    RUN(test_set_null_fallback_restores_default);
    RUN(test_registered_handler_beats_fallback);

    /* Boundaries */
    RUN(test_register_at_linux_range_max);
    RUN(test_register_at_vm_range_boundaries);

    /* Null safety */
    RUN(test_dispatch_null_safe);

    return TEST_SUITE_RESULT();
}
