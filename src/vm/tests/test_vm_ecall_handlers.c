/* Tests for the cpu-only ECALL handlers:
 *   vm_handle_exit
 *   vm_handle_self
 *   vm_handle_yield
 *   vm_handle_critical_enter
 *   vm_handle_critical_exit
 *
 * Plus the bulk-installer vm_ecall_install_cpu_handlers.
 * Public domain (CC0). No warranty. */

#include "test_runner.h"
#include "vm/vm_ecall.h"
#include "vm/vm_core.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* Forward declarations of the handler functions and installer
 * (they're defined in vm_ecall_handlers.c but not currently
 * declared in any header). */
void vm_handle_exit(VmCpu *cpu, void *system);
void vm_handle_self(VmCpu *cpu, void *system);
void vm_handle_yield(VmCpu *cpu, void *system);
void vm_handle_critical_enter(VmCpu *cpu, void *system);
void vm_handle_critical_exit(VmCpu *cpu, void *system);
bool vm_ecall_install_cpu_handlers(VmEcallRouter *r);

/* ============================================================
 *  SYS_EXIT
 * ============================================================ */

static void test_exit_sets_halted(void) {
    VmCpu cpu;
    vm_init(&cpu, 0);
    ASSERT(!cpu.halted);
    vm_handle_exit(&cpu, NULL);
    ASSERT(cpu.halted);
}

static void test_exit_null_safe(void) {
    vm_handle_exit(NULL, NULL);
    /* No crash = pass */
}

/* ============================================================
 *  SYS_SELF
 * ============================================================ */

static void test_self_returns_vm_id(void) {
    VmCpu cpu;
    vm_init(&cpu, 42);
    vm_handle_self(&cpu, NULL);
    ASSERT_EQ_INT(42, (int)cpu.regs[VM_REG_A0]);
}

static void test_self_works_for_zero_id(void) {
    VmCpu cpu;
    vm_init(&cpu, 0);
    cpu.regs[VM_REG_A0] = 0xCCCC;   /* pre-fill to verify write */
    vm_handle_self(&cpu, NULL);
    ASSERT_EQ_INT(0, (int)cpu.regs[VM_REG_A0]);
}

static void test_self_for_large_id(void) {
    VmCpu cpu;
    vm_init(&cpu, 0xFFFF);   /* maximum uint16_t */
    vm_handle_self(&cpu, NULL);
    ASSERT_EQ_INT(0xFFFF, (int)cpu.regs[VM_REG_A0]);
}

/* ============================================================
 *  SYS_YIELD
 * ============================================================ */

static void test_yield_sets_block_reason(void) {
    VmCpu cpu;
    vm_init(&cpu, 0);
    ASSERT_EQ_INT(BLOCK_NONE, cpu.block_reason);
    vm_handle_yield(&cpu, NULL);
    ASSERT_EQ_INT(BLOCK_YIELDED, cpu.block_reason);
    ASSERT_EQ_INT(0, (int)cpu.regs[VM_REG_A0]);
}

static void test_yield_does_not_halt(void) {
    VmCpu cpu;
    vm_init(&cpu, 0);
    vm_handle_yield(&cpu, NULL);
    ASSERT(!cpu.halted);
}

/* ============================================================
 *  SYS_CRITICAL_ENTER
 * ============================================================ */

static void test_critical_enter_succeeds(void) {
    VmCpu cpu;
    vm_init(&cpu, 0);
    ASSERT(!cpu.in_critical);
    vm_handle_critical_enter(&cpu, NULL);
    ASSERT(cpu.in_critical);
    ASSERT_EQ_INT(0, (int)cpu.regs[VM_REG_A0]);
}

static void test_critical_enter_rejects_nested(void) {
    VmCpu cpu;
    vm_init(&cpu, 0);
    vm_handle_critical_enter(&cpu, NULL);
    /* Try to enter again — should get -EBUSY */
    vm_handle_critical_enter(&cpu, NULL);
    ASSERT(cpu.in_critical);   /* still in critical */
    ASSERT_EQ_INT((int)(uint32_t)-VM_EBUSY, (int)cpu.regs[VM_REG_A0]);
}

/* ============================================================
 *  SYS_CRITICAL_EXIT
 * ============================================================ */

static void test_critical_exit_succeeds(void) {
    VmCpu cpu;
    vm_init(&cpu, 0);
    cpu.in_critical = true;
    vm_handle_critical_exit(&cpu, NULL);
    ASSERT(!cpu.in_critical);
    ASSERT_EQ_INT(0, (int)cpu.regs[VM_REG_A0]);
}

static void test_critical_exit_rejects_when_not_in_critical(void) {
    VmCpu cpu;
    vm_init(&cpu, 0);
    /* Not in critical — exit should fail */
    vm_handle_critical_exit(&cpu, NULL);
    ASSERT(!cpu.in_critical);
    ASSERT_EQ_INT((int)(uint32_t)-VM_EINVAL, (int)cpu.regs[VM_REG_A0]);
}

static void test_critical_enter_exit_round_trip(void) {
    VmCpu cpu;
    vm_init(&cpu, 0);

    vm_handle_critical_enter(&cpu, NULL);
    ASSERT(cpu.in_critical);

    vm_handle_critical_exit(&cpu, NULL);
    ASSERT(!cpu.in_critical);

    /* Can re-enter after exit */
    vm_handle_critical_enter(&cpu, NULL);
    ASSERT(cpu.in_critical);
}

/* ============================================================
 *  Bulk installer
 * ============================================================ */

static void test_install_registers_all(void) {
    VmEcallRouter r;
    vm_ecall_router_init(&r);
    ASSERT(vm_ecall_install_cpu_handlers(&r));

    /* Verify each handler is in the right slot */
    ASSERT(r.linux_slots[SYS_EXIT] == vm_handle_exit);
    ASSERT(r.vm_slots[SYS_SELF - VM_ECALL_VM_RANGE_START]
           == vm_handle_self);
    ASSERT(r.vm_slots[SYS_YIELD - VM_ECALL_VM_RANGE_START]
           == vm_handle_yield);
    ASSERT(r.vm_slots[SYS_CRITICAL_ENTER - VM_ECALL_VM_RANGE_START]
           == vm_handle_critical_enter);
    ASSERT(r.vm_slots[SYS_CRITICAL_EXIT - VM_ECALL_VM_RANGE_START]
           == vm_handle_critical_exit);
}

static void test_install_rolls_back_on_conflict(void) {
    /* Pre-register something at SYS_YIELD so install fails halfway
     * through. Verify that the handlers registered before the
     * failure are removed (rolled back). */
    VmEcallRouter r;
    vm_ecall_router_init(&r);

    /* Use any non-NULL handler for the conflict */
    ASSERT(vm_ecall_register(&r, SYS_YIELD, vm_handle_self));

    /* Now install should fail */
    ASSERT(!vm_ecall_install_cpu_handlers(&r));

    /* The handlers registered before SYS_YIELD (EXIT, SELF) should
     * have been removed by the rollback */
    ASSERT(r.linux_slots[SYS_EXIT] == NULL);
    ASSERT(r.vm_slots[SYS_SELF - VM_ECALL_VM_RANGE_START] == NULL);

    /* The pre-existing conflict handler should remain */
    ASSERT(r.vm_slots[SYS_YIELD - VM_ECALL_VM_RANGE_START]
           == vm_handle_self);
}

static void test_install_null_safe(void) {
    ASSERT(!vm_ecall_install_cpu_handlers(NULL));
}

/* ============================================================
 *  End-to-end: install + dispatch via router
 * ============================================================ */

static void test_end_to_end_exit(void) {
    VmEcallRouter r;
    vm_ecall_router_init(&r);
    vm_ecall_install_cpu_handlers(&r);

    VmCpu cpu;
    vm_init(&cpu, 0);
    cpu.regs[VM_REG_A7] = SYS_EXIT;

    vm_ecall_dispatch(&r, &cpu, NULL);
    ASSERT(cpu.halted);
}

static void test_end_to_end_self(void) {
    VmEcallRouter r;
    vm_ecall_router_init(&r);
    vm_ecall_install_cpu_handlers(&r);

    VmCpu cpu;
    vm_init(&cpu, 17);
    cpu.regs[VM_REG_A7] = SYS_SELF;

    vm_ecall_dispatch(&r, &cpu, NULL);
    ASSERT_EQ_INT(17, (int)cpu.regs[VM_REG_A0]);
}

static void test_end_to_end_yield(void) {
    VmEcallRouter r;
    vm_ecall_router_init(&r);
    vm_ecall_install_cpu_handlers(&r);

    VmCpu cpu;
    vm_init(&cpu, 0);
    cpu.regs[VM_REG_A7] = SYS_YIELD;

    vm_ecall_dispatch(&r, &cpu, NULL);
    ASSERT_EQ_INT(BLOCK_YIELDED, cpu.block_reason);
    ASSERT_EQ_INT(0, (int)cpu.regs[VM_REG_A0]);
}

static void test_end_to_end_critical_cycle(void) {
    VmEcallRouter r;
    vm_ecall_router_init(&r);
    vm_ecall_install_cpu_handlers(&r);

    VmCpu cpu;
    vm_init(&cpu, 0);

    /* Enter */
    cpu.regs[VM_REG_A7] = SYS_CRITICAL_ENTER;
    vm_ecall_dispatch(&r, &cpu, NULL);
    ASSERT_EQ_INT(0, (int)cpu.regs[VM_REG_A0]);
    ASSERT(cpu.in_critical);

    /* Exit */
    cpu.regs[VM_REG_A7] = SYS_CRITICAL_EXIT;
    vm_ecall_dispatch(&r, &cpu, NULL);
    ASSERT_EQ_INT(0, (int)cpu.regs[VM_REG_A0]);
    ASSERT(!cpu.in_critical);
}

/* ============================================================
 *  End-to-end with the actual dispatcher: emit an ECALL
 *  instruction, run vm_step, see TRAP_ECALL, dispatch.
 * ============================================================ */

static void test_dispatcher_to_handler_pipeline(void) {
    /* Plant: ecall (a7=SYS_SELF=1024)
     * After vm_step returns VM_STEP_ECALL, dispatch the handler.
     * Verify a0 holds the vm_id. */
    VmEcallRouter r;
    vm_ecall_router_init(&r);
    vm_ecall_install_cpu_handlers(&r);

    static uint8_t code[] = { 0x73, 0x00, 0x00, 0x00 };   /* ECALL */
    VmCpu cpu;
    vm_init(&cpu, 99);
    cpu.regions[VM_REGION_CODE].base = code;
    cpu.regions[VM_REGION_CODE].length = sizeof(code);

    cpu.regs[VM_REG_A7] = SYS_SELF;

    uint32_t used = 0;
    VmStepResult result = vm_step(&cpu, 1, &used);
    ASSERT_EQ_INT(VM_STEP_ECALL, result);
    /* PC advanced past the ecall */
    ASSERT_EQ_INT(4, (int)cpu.pc);

    /* Now dispatch */
    vm_ecall_dispatch(&r, &cpu, NULL);
    ASSERT_EQ_INT(99, (int)cpu.regs[VM_REG_A0]);
}

static void test_dispatcher_to_unregistered_returns_enosys(void) {
    /* ECALL with a7 = some unregistered number → -ENOSYS in a0 */
    VmEcallRouter r;
    vm_ecall_router_init(&r);
    /* Don't install anything */

    static uint8_t code[] = { 0x73, 0x00, 0x00, 0x00 };
    VmCpu cpu;
    vm_init(&cpu, 0);
    cpu.regions[VM_REGION_CODE].base = code;
    cpu.regions[VM_REGION_CODE].length = sizeof(code);
    cpu.regs[VM_REG_A7] = SYS_SELF;

    vm_step(&cpu, 1, NULL);
    vm_ecall_dispatch(&r, &cpu, NULL);
    ASSERT_EQ_INT((int)(uint32_t)-VM_ENOSYS, (int)cpu.regs[VM_REG_A0]);
}

/* ============================================================
 *  Test runner
 * ============================================================ */

int main(void) {
    TEST_SUITE("vm_ecall_handlers");

    /* SYS_EXIT */
    RUN(test_exit_sets_halted);
    RUN(test_exit_null_safe);

    /* SYS_SELF */
    RUN(test_self_returns_vm_id);
    RUN(test_self_works_for_zero_id);
    RUN(test_self_for_large_id);

    /* SYS_YIELD */
    RUN(test_yield_sets_block_reason);
    RUN(test_yield_does_not_halt);

    /* SYS_CRITICAL_ENTER */
    RUN(test_critical_enter_succeeds);
    RUN(test_critical_enter_rejects_nested);

    /* SYS_CRITICAL_EXIT */
    RUN(test_critical_exit_succeeds);
    RUN(test_critical_exit_rejects_when_not_in_critical);
    RUN(test_critical_enter_exit_round_trip);

    /* Installer */
    RUN(test_install_registers_all);
    RUN(test_install_rolls_back_on_conflict);
    RUN(test_install_null_safe);

    /* End-to-end via router */
    RUN(test_end_to_end_exit);
    RUN(test_end_to_end_self);
    RUN(test_end_to_end_yield);
    RUN(test_end_to_end_critical_cycle);

    /* End-to-end via real dispatcher */
    RUN(test_dispatcher_to_handler_pipeline);
    RUN(test_dispatcher_to_unregistered_returns_enosys);

    return TEST_SUITE_RESULT();
}
