/* ============================================================
 *  host_runloop.c — multi-session TCP run loop.
 *
 *  See host_runloop.h for the contract.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "host_runloop.h"

#ifdef TCP_MODE_SUPPORTED

#include "vm/vm_host_tui.h"
#include "vm/vm_sched.h"
#include "vm/host_platform.h"

#include <stdio.h>

bool host_runloop_tcp_setup(HostRunloopTcp *rl,
                            const int *ports, int n_ports) {
    rl->n_ports = n_ports;
    for (int i = 0; i < MAX_TCP_PORTS; i++) {
        rl->slot_active[i] = false;
        rl->slot_vm[i]     = UINT16_MAX;
    }

    /* Per-port TUI session slot. The TUI service walks vm-id to slot;
     * the pool size needs to match what main() reserves. */
    static VmTuiSession tui_pool[MAX_TCP_PORTS];
    vm_host_tui_set_pool(tui_pool, (unsigned)n_ports);

    for (int i = 0; i < n_ports; i++) {
        rl->ctxs[i].listen_fd   = tcp_listen(ports[i]);
        rl->ctxs[i].client_fd   = TCP_SOCK_INVALID;
        rl->ctxs[i].port        = ports[i];
        rl->ctxs[i].prev_was_cr = 0;
        rl->ctxs[i].iac_state   = 0;
        rl->ctxs[i].iac_verb    = 0;
        if (rl->ctxs[i].listen_fd == TCP_SOCK_INVALID) {
            fprintf(stderr, "host: failed to listen on port %d\n", ports[i]);
            return false;
        }
        rl->transports[i].read_nonblock = tcp_t_read;
        rl->transports[i].write         = tcp_t_write;
        rl->transports[i].flush         = tcp_t_flush;
        rl->transports[i].set_raw       = tcp_t_set_raw;
        rl->transports[i].close         = tcp_t_close;
        rl->transports[i].is_terminal   = true;
        rl->transports[i].ctx           = &rl->ctxs[i];
        fprintf(stderr, "host: listening on TCP port %d "
                "(connect: nc localhost %d)\n", ports[i], ports[i]);
    }
    fprintf(stderr, "host: %d session(s) ready; connect clients now.\n",
            n_ports);
    fprintf(stderr,
        "host: PuTTY - connection type Raw OR Telnet both work\n"
        "      (the host absorbs Telnet negotiation). For the\n"
        "      cleanest line editing, under Terminal set\n"
        "      'Local echo' = Force off and 'Local line editing'\n"
        "      = Force off, else PuTTY echoes your own keystrokes\n"
        "      and buffers lines instead of sending keys live.\n");
    fprintf(stderr, "host: ports stay open - reconnect any time. "
                    "Ctrl-C to stop the host.\n");
    fflush(stderr);
    return true;
}

/* Reap exited shells: a slot whose VM is gone (the reap in
 * vm_system_step unloaded it) goes back to LISTENING so its port
 * accepts a new client. */
static void reap_slots(HostRunloopTcp *rl, VmSystem *sys) {
    for (int i = 0; i < rl->n_ports; i++) {
        if (!rl->slot_active[i]) continue;
        if (vm_sched_get(sys->sched, rl->slot_vm[i]) == NULL) {
            if (rl->ctxs[i].client_fd != TCP_SOCK_INVALID) {
                tcp_close(rl->ctxs[i].client_fd);
                rl->ctxs[i].client_fd = TCP_SOCK_INVALID;
            }
            rl->ctxs[i].prev_was_cr = 0;
            rl->ctxs[i].iac_state   = 0;
            rl->slot_active[i] = false;
            rl->slot_vm[i]     = UINT16_MAX;
            fprintf(stderr, "host: [:%d] session ended; "
                    "port open for reconnection\n", rl->ctxs[i].port);
            fflush(stderr);
        }
    }
}

/* Accept new clients on idle slots and spawn a shell on each. */
static void accept_slots(HostRunloopTcp *rl, VmSystem *sys,
                         const uint8_t *elf, size_t elf_size,
                         uint32_t data_kb, VmBacking backing) {
    for (int i = 0; i < rl->n_ports; i++) {
        if (rl->slot_active[i]) continue;
        if (!tcp_try_accept(&rl->ctxs[i])) continue;

        VmLoadVmResult lr = vm_system_load_vm(
            sys, elf, elf_size, data_kb * 1024,
            backing, backing);
        if (lr.code != VM_SYS_OK) {
            fprintf(stderr, "host: [:%d] load failed (code=%d)\n",
                    rl->ctxs[i].port, lr.code);
            tcp_close(rl->ctxs[i].client_fd);
            rl->ctxs[i].client_fd = TCP_SOCK_INVALID;
            continue;
        }
        rl->slot_active[i] = true;
        rl->slot_vm[i]     = (uint16_t)lr.assigned_vm_id;
        vm_host_set_transport_for_vm(lr.assigned_vm_id,
                                     &rl->transports[i]);
        fprintf(stderr, "host: [:%d] shell spawned (vm %u)\n",
                rl->ctxs[i].port, (unsigned)lr.assigned_vm_id);
        fflush(stderr);
    }
}

void host_runloop_tcp_run(HostRunloopTcp *rl, VmSystem *sys,
                          const uint8_t *elf, size_t elf_size,
                          uint32_t data_kb, VmBacking backing) {
    for (;;) {
        if (host_platform_stop_requested()) {
            fprintf(stderr, "\nhost: stop requested, shutting down.\n");
            break;
        }

        reap_slots(rl, sys);
        accept_slots(rl, sys, elf, elf_size, data_kb, backing);

        /* Run the scheduler one step if any shell is live; otherwise
         * sleep briefly so we don't busy-spin while waiting for
         * connections (and so Ctrl-C is responsive — a hot loop can
         * delay signal handling on Cygwin). */
        int n_active = 0;
        for (int i = 0; i < rl->n_ports; i++) {
            if (rl->slot_active[i]) n_active++;
        }

        if (n_active > 0) {
            VmSchedStepResult r = vm_system_step(sys);
            if (r != VM_SCHED_RAN) {
                /* IDLE (all shells blocked on input) or ALL_HALTED
                 * (nothing ran this step) — yield the CPU briefly. */
                host_platform_sleep_ms(5);
            }
        } else {
            host_platform_sleep_ms(10);
        }
    }
}

void host_runloop_tcp_teardown(HostRunloopTcp *rl) {
    for (int i = 0; i < rl->n_ports; i++) {
        if (rl->transports[i].close) {
            rl->transports[i].close(&rl->transports[i]);
        }
        if (rl->ctxs[i].listen_fd != TCP_SOCK_INVALID) {
            tcp_close(rl->ctxs[i].listen_fd);
            rl->ctxs[i].listen_fd = TCP_SOCK_INVALID;
        }
    }
    tcp_global_shutdown();
}

#else  /* !TCP_MODE_SUPPORTED */

typedef int host_runloop_unused_on_this_platform;

#endif /* TCP_MODE_SUPPORTED */
