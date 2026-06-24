/* ============================================================
 *  vm_init.h — bring up the VM system inside mgapi.
 *
 *  Stage 3a: stands up VmSystem, installs the fs + stdio bridges,
 *  mounts /td0/ (small AXI-shape scratch) and /cart/ (PSRAM bulk),
 *  and loads the embedded shell ELF as the first VM. mgapi_step
 *  drives the scheduler one quantum per call.
 *
 *  Stage 3b will add SYS_COPRO_* ecalls; 3c will add SYS_L2_*.
 *  TCP/PuTTY transport binding is stage 4.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MGAPI_VM_INIT_H
#define MGAPI_VM_INIT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Set the L2 backing applied to every loaded VM. Call BEFORE
 * mgapi_vm_init; setting after init has no effect on the already-
 * loaded shell. base+size are the L2 PSRAM slice; NULL/0 leaves the
 * L2 sub-region absent (guest L2 accesses fault). */
void mgapi_vm_set_l2_backing(void *base, size_t size);

/* If set to nonzero before mgapi_vm_init, the VM system is built
 * WITHOUT a default stdio transport. The shell loads but its first
 * sys_read returns EOF, so the shell exits immediately. Used by
 * embedded contexts (bsnes-plus, MCU firmware) where the embedder's
 * stdin must not block on console reads. Default 0 = install stdio
 * (preserves the standalone-shell behavior). */
void mgapi_vm_set_disable_default_stdio(int flag);

/* Set the program the shell autostarts on boot — typically read from the
 * loaded .sfc cart's MGBOOT tag, making that cart a bootable "game" (the path
 * is seeded into /td0/etc/autostart, which the shell runs at _start). An empty
 * or NULL path means "no cart-specified program"; vm_init then falls back to
 * the connect-PuTTY boot banner. Call BEFORE mgapi_vm_init; the string is
 * copied. */
void mgapi_vm_set_autostart_path(const char *path);

/* Bring up VmSystem, install bridges, mount both volumes, load the
 * embedded shell.elf. `cart_volume_handle` is the trashfs volume
 * stage 2c stood up; we register it as the "cart" mount. Returns 0
 * on success, negative errno on failure. */
int  mgapi_vm_init(void *cart_volume_handle);

/* Tear down. Halts every VM, destroys the system, releases the
 * volume reference. */
void mgapi_vm_shutdown(void);

/* Advance the scheduler by one quantum. Returns true if anything
 * ran, false if all VMs are halted or idle. mgapi_step calls this
 * once per tick. */
bool mgapi_vm_step(void);

/* The shell VM's assigned ID, or UINT16_MAX if no shell is loaded.
 * The TCP listener uses this to bind its transport to the shell. */
uint16_t mgapi_vm_shell_id(void);

/* Dev: load an ELF, run it to completion, return its exit code.
 * Used by the host test to verify guest-side stage 3c (L2) ecalls
 * without going through the interactive shell. Returns the guest's
 * sys_exit code on clean halt, or a negative errno on load failure.
 *
 * Blocks the calling thread until the spawned VM halts. The shell
 * VM (already running) continues to run during this — both VMs
 * share the scheduler's quantum.
 */
int mgapi_dev_spawn_elf_and_wait(const void *elf, uint32_t elf_size);

/* Like spawn_and_wait but for guests that don't naturally exit (the
 * menu loop, the shell, anything game-shaped). Loads the ELF, runs
 * the scheduler for `max_steps` quanta, halts and unloads the VM.
 * Used by host tests that want to peek at state the guest staged.
 *
 * Returns 0 on success, -EIO on load failure, -EAGAIN if the VM
 * system isn't up. The caller is expected to inspect global state
 * (e.g. cart_window) after this returns. */
int mgapi_dev_spawn_elf_for_steps(const void *elf, uint32_t elf_size,
                                   uint32_t max_steps);

/* Dev: is a VM loaded and the shell ELF embedded? Used by the host
 * test to confirm stage 3a's wiring. */
typedef struct {
    bool   has_shell_elf;
    size_t shell_elf_bytes;
    bool   vm_system_alive;
    bool   shell_loaded;
    uint16_t shell_vm_id;
} MgapiVmStats;
void mgapi_vm_stats(MgapiVmStats *out);

#ifdef __cplusplus
}
#endif

#endif /* MGAPI_VM_INIT_H */
