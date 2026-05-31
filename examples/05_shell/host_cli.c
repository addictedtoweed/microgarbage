/* ============================================================
 *  host_cli.c — argv parser.
 *
 *  See host_cli.h for the contract.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "host_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void host_cli_set_defaults(HostCli *cli) {
    cli->elf_path             = NULL;
    cli->host_fs_root         = "host_files";
    cli->host_fs_root_explicit = false;
    cli->host_fs_writable     = false;
    cli->host_fs_disabled     = false;
    cli->want_pty             = false;
    cli->n_tcp_ports          = 0;
    cli->cfg_path             = NULL;
    cli->no_config            = false;

    /* -1 sentinel for "user didn't set this; vm.cfg/defaults win". */
    cli->cli_local_kb         = -1;
    cli->cli_shared_kb        = -1;
    cli->cli_max_vms          = -1;
    cli->cli_spawn_data_kb    = -1;
    cli->cli_raw_mode         = -1;
}

/* Print the usage block to stderr. Matches what main() used to print
 * inline. Kept in one place so the doc comment in host_cli.h and the
 * runtime --help output stay in sync (only need to edit once). */
static void print_usage(void) {
    fprintf(stderr, "  --config=<path>     load config from <path>\n");
    fprintf(stderr, "  --no-config         skip ./vm.cfg even if present\n");
    fprintf(stderr, "  --local-kb=<N>      local-slab size in KB\n");
    fprintf(stderr, "  --shared-kb=<N>     shared-slab size in KB\n");
    fprintf(stderr, "  --max-vms=<N>       max concurrent VMs\n");
    fprintf(stderr, "  --spawn-data-kb=<N> per-spawn data region in KB\n");
    fprintf(stderr, "  --raw=on|off        toggle raw-mode stdin\n");
    fprintf(stderr, "  --host-fs=<path>    mount path as /host (default: ./host_files)\n");
    fprintf(stderr, "  --host-fs-rw        allow writes to /host (default: read-only)\n");
    fprintf(stderr, "  --no-host-fs        disable /host mount\n");
    fprintf(stderr, "  --pty               route stdio through a POSIX pty (Linux/Cygwin)\n");
    fprintf(stderr, "  --tcp=<port>        listen on TCP port; first client gets the shell\n");
}

bool host_cli_parse(int argc, char **argv, HostCli *cli) {
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--config=", 9) == 0) {
            cli->cfg_path = argv[i] + 9;
        } else if (strcmp(argv[i], "--no-config") == 0) {
            cli->no_config = true;
        } else if (strncmp(argv[i], "--local-kb=", 11) == 0) {
            cli->cli_local_kb = strtol(argv[i] + 11, NULL, 10);
        } else if (strncmp(argv[i], "--shared-kb=", 12) == 0) {
            cli->cli_shared_kb = strtol(argv[i] + 12, NULL, 10);
        } else if (strncmp(argv[i], "--max-vms=", 10) == 0) {
            cli->cli_max_vms = strtol(argv[i] + 10, NULL, 10);
        } else if (strncmp(argv[i], "--spawn-data-kb=", 16) == 0) {
            cli->cli_spawn_data_kb = strtol(argv[i] + 16, NULL, 10);
        } else if (strncmp(argv[i], "--raw=", 6) == 0) {
            const char *v = argv[i] + 6;
            if (strcmp(v, "on") == 0 || strcmp(v, "true") == 0 ||
                strcmp(v, "yes") == 0 || strcmp(v, "1") == 0) {
                cli->cli_raw_mode = 1;
            } else if (strcmp(v, "off") == 0 || strcmp(v, "false") == 0 ||
                       strcmp(v, "no") == 0 || strcmp(v, "0") == 0) {
                cli->cli_raw_mode = 0;
            } else {
                fprintf(stderr, "host: --raw expects on|off (got '%s')\n", v);
                return false;
            }
        } else if (strncmp(argv[i], "--host-fs=", 10) == 0) {
            cli->host_fs_root = argv[i] + 10;
            cli->host_fs_root_explicit = true;
        } else if (strcmp(argv[i], "--host-fs-rw") == 0) {
            cli->host_fs_writable = true;
        } else if (strcmp(argv[i], "--no-host-fs") == 0) {
            cli->host_fs_disabled = true;
        } else if (strcmp(argv[i], "--pty") == 0) {
            cli->want_pty = true;
        } else if (strncmp(argv[i], "--tcp=", 6) == 0) {
            long p = strtol(argv[i] + 6, NULL, 10);
            if (p < 1 || p > 65535) {
                fprintf(stderr, "host: --tcp port out of range (1..65535)\n");
                return false;
            }
            if (cli->n_tcp_ports >= MAX_TCP_PORTS) {
                fprintf(stderr, "host: too many --tcp ports (max %d)\n",
                        MAX_TCP_PORTS);
                return false;
            }
            cli->tcp_ports[cli->n_tcp_ports++] = (int)p;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "host: unknown option '%s'\n", argv[i]);
            print_usage();
            return false;
        } else if (!cli->elf_path) {
            cli->elf_path = argv[i];
        } else {
            fprintf(stderr, "host: extra positional argument '%s'\n", argv[i]);
            return false;
        }
    }
    return true;
}

bool host_cli_apply_to_config(const HostCli *cli, HostConfig *hc) {
    if (cli->cli_local_kb >= 0) {
        if (cli->cli_local_kb < 32 ||
            cli->cli_local_kb > (long)(LOCAL_BYTES / 1024)) {
            fprintf(stderr, "host: --local-kb=%ld out of range (32..%llu)\n",
                    cli->cli_local_kb,
                    (unsigned long long)(LOCAL_BYTES / 1024));
            return false;
        }
        hc->local_bytes = (size_t)cli->cli_local_kb * 1024;
    }
    if (cli->cli_shared_kb >= 0) {
        if (cli->cli_shared_kb < 8 ||
            cli->cli_shared_kb > (long)(SHARED_BYTES / 1024)) {
            fprintf(stderr, "host: --shared-kb=%ld out of range (8..%llu)\n",
                    cli->cli_shared_kb,
                    (unsigned long long)(SHARED_BYTES / 1024));
            return false;
        }
        hc->shared_bytes = (size_t)cli->cli_shared_kb * 1024;
    }
    if (cli->cli_max_vms >= 0) {
        if (cli->cli_max_vms < 1 || cli->cli_max_vms > 16) {
            fprintf(stderr, "host: --max-vms=%ld out of range (1..16)\n",
                    cli->cli_max_vms);
            return false;
        }
        hc->max_vms = (uint16_t)cli->cli_max_vms;
    }
    if (cli->cli_spawn_data_kb >= 0) {
        if (cli->cli_spawn_data_kb < 1 || cli->cli_spawn_data_kb > 256) {
            fprintf(stderr, "host: --spawn-data-kb=%ld out of range (1..256)\n",
                    cli->cli_spawn_data_kb);
            return false;
        }
        hc->spawn_data_kb = (uint16_t)cli->cli_spawn_data_kb;
    }
    if (cli->cli_raw_mode != -1) {
        hc->raw_mode = (cli->cli_raw_mode != 0);
    }
    return true;
}
