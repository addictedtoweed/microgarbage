/* ============================================================
 *  host_cli.h — argv parser for the shell host.
 *
 *  The CLI surface is "config + overrides": the same knobs that
 *  vm.cfg sets, plus a few transport / mount controls vm.cfg has
 *  no business deciding (--pty, --tcp=, --host-fs=). The struct
 *  HostCli captures both layers in one bundle so main() pipes
 *  exactly one value around instead of a dozen locals.
 *
 *  Two entry points:
 *
 *    host_cli_parse(argc, argv, cli)
 *      Walks argv. On --help, an unknown option, or a malformed
 *      value, prints usage / a line about the failure to stderr
 *      and returns false. On success returns true and `cli` is
 *      populated with whatever was set; un-set fields keep their
 *      defaults (sentinel -1 for "no CLI override").
 *
 *    host_cli_apply_to_config(cli, hc)
 *      Applies the CLI integer/bool overrides to a HostConfig
 *      AFTER vm.cfg has been loaded into it. Each value is range-
 *      checked so an out-of-range CLI override fails cleanly
 *      instead of silently clamping. The transport / mount fields
 *      (tcp_ports, want_pty, host_fs_*) live on HostCli only —
 *      they have no vm.cfg counterpart.
 *
 *  Layering (built-in defaults < vm.cfg < CLI) lives in main():
 *  host_config_set_defaults -> host_config_load -> host_cli_apply.
 *  CLI wins ties; vm.cfg is "persistent defaults".
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef HOST_CLI_H
#define HOST_CLI_H

#include <stdbool.h>

#include "host_config.h"

/* Cap on simultaneous --tcp= listeners. main() allocates a parallel
 * TcpCtx array of this size, so changing it is an ABI-ish bump:
 * audit run-loop polling cost (O(N) per frame) when raising it. */
#define MAX_TCP_PORTS 16

typedef struct {
    /* Positional ELF path. NULL = use the baked-in shell image. */
    const char *elf_path;

    /* /host mount (filesystem bridge to the desktop). */
    const char *host_fs_root;          /* default "host_files" */
    bool        host_fs_root_explicit; /* user passed --host-fs= */
    bool        host_fs_writable;      /* --host-fs-rw */
    bool        host_fs_disabled;      /* --no-host-fs */

    /* Transports. --pty is single-instance; --tcp= is multi. main()
     * rejects the combination. */
    bool        want_pty;              /* --pty */
    int         tcp_ports[MAX_TCP_PORTS];
    int         n_tcp_ports;

    /* vm.cfg path. NULL = try ./vm.cfg silently. */
    const char *cfg_path;              /* --config= */
    bool        no_config;             /* --no-config */

    /* CLI overrides for HostConfig fields. -1 sentinel = not set. */
    long        cli_local_kb;
    long        cli_shared_kb;
    long        cli_max_vms;
    long        cli_spawn_data_kb;
    int         cli_raw_mode;          /* 0=off, 1=on, -1=unset */
} HostCli;

/* Reset cli to "no flags set, defaults applied" state. Call before
 * host_cli_parse. */
void host_cli_set_defaults(HostCli *cli);

/* Walk argv into *cli. Returns false on parse failure (usage printed
 * to stderr); the caller should exit non-zero. */
bool host_cli_parse(int argc, char **argv, HostCli *cli);

/* Apply CLI integer/bool overrides to *hc. Range-checks each value;
 * prints to stderr and returns false on out-of-range. Call AFTER
 * vm.cfg has been loaded so the CLI wins ties. */
bool host_cli_apply_to_config(const HostCli *cli, HostConfig *hc);

#endif /* HOST_CLI_H */
