/* ============================================================
 *  host_config.c — vm.cfg loader.
 *
 *  See host_config.h for the contract.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "host_config.h"

#include "util/inicfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void host_config_set_defaults(HostConfig *hc) {
    hc->local_bytes   = LOCAL_BYTES;
    hc->shared_bytes  = SHARED_BYTES;
    hc->max_vms       = 4;
    hc->spawn_data_kb = 64;
    hc->raw_mode      = true;
    hc->mount_count   = 0;
}

/* Find or create a mount entry by name. Returns NULL on cap-exceeded. */
static HostMount *mount_get_or_create(HostConfig *hc, const char *name) {
    for (unsigned i = 0; i < hc->mount_count; i++) {
        if (strcmp(hc->mounts[i].name, name) == 0) return &hc->mounts[i];
    }
    if (hc->mount_count >= HOST_MOUNT_MAX) return NULL;
    HostMount *m = &hc->mounts[hc->mount_count++];
    memset(m, 0, sizeof(*m));
    size_t n = strlen(name);
    if (n >= sizeof(m->name)) n = sizeof(m->name) - 1;
    memcpy(m->name, name, n);
    m->name[n] = '\0';
    return m;
}

/* Apply an IniCfg's settings to *hc. Unknown keys produce a warning
 * on stderr but don't fail the load — forward-compat matters more
 * than strictness for a config file the user edits by hand. */
static bool apply_inicfg(const IniCfg *cfg, HostConfig *hc) {
    long lv;
    bool bv;

    if (inicfg_get_int(cfg, "memory", "local_kb", &lv)) {
        if (lv < 32 || lv > (long)(LOCAL_BYTES / 1024)) {
            fprintf(stderr, "vm.cfg: [memory] local_kb=%ld out of range "
                    "(32..%llu)\n", lv,
                    (unsigned long long)(LOCAL_BYTES / 1024));
            return false;
        }
        hc->local_bytes = (size_t)lv * 1024;
    }
    if (inicfg_get_int(cfg, "memory", "shared_kb", &lv)) {
        if (lv < 8 || lv > (long)(SHARED_BYTES / 1024)) {
            fprintf(stderr, "vm.cfg: [memory] shared_kb=%ld out of range "
                    "(8..%llu)\n", lv,
                    (unsigned long long)(SHARED_BYTES / 1024));
            return false;
        }
        hc->shared_bytes = (size_t)lv * 1024;
    }
    if (inicfg_get_int(cfg, "memory", "max_vms", &lv)) {
        if (lv < 1 || lv > 16) {
            fprintf(stderr, "vm.cfg: [memory] max_vms=%ld out of range "
                    "(1..16)\n", lv);
            return false;
        }
        hc->max_vms = (uint16_t)lv;
    }
    if (inicfg_get_int(cfg, "memory", "spawn_data_kb", &lv)) {
        if (lv < 1 || lv > 256) {
            fprintf(stderr, "vm.cfg: [memory] spawn_data_kb=%ld out of "
                    "range (1..256)\n", lv);
            return false;
        }
        hc->spawn_data_kb = (uint16_t)lv;
    }
    if (inicfg_get_bool(cfg, "stdio", "raw_mode", &bv)) {
        hc->raw_mode = bv;
    }

    /* Walk all entries looking for [mount.<name>] sections. Each such
     * section defines one mount. Key bindings within the section:
     *
     *   type     = host | tmpfs | sd
     *   path     = <dir>    (host only; absolute or relative)
     *   writable = bool     (host only; default false)
     *   size_kb  = <int>    (tmpfs / sd only; default 128)
     */
    for (size_t i = 0; i < cfg->count; i++) {
        const char *s = cfg->entries[i].section;
        if (strncmp(s, "mount.", 6) != 0) continue;
        const char *name = s + 6;
        if (*name == '\0') {
            fprintf(stderr, "vm.cfg: line %u: empty mount name in [mount.]\n",
                    cfg->entries[i].line);
            return false;
        }
        HostMount *m = mount_get_or_create(hc, name);
        if (!m) {
            fprintf(stderr, "vm.cfg: line %u: too many [mount.*] sections "
                    "(max %d)\n", cfg->entries[i].line, HOST_MOUNT_MAX);
            return false;
        }
        const char *k = cfg->entries[i].key;
        const char *v = cfg->entries[i].value;
        if (strcmp(k, "type") == 0) {
            if (strcmp(v, "host") == 0)       m->kind = HOST_MOUNT_HOST;
            else if (strcmp(v, "tmpfs") == 0) m->kind = HOST_MOUNT_TMPFS;
            else if (strcmp(v, "sd") == 0)    m->kind = HOST_MOUNT_SD;
            else {
                fprintf(stderr, "vm.cfg: line %u: [mount.%s] unknown type "
                        "'%s' (expected 'host', 'tmpfs', or 'sd')\n",
                        cfg->entries[i].line, name, v);
                return false;
            }
        } else if (strcmp(k, "path") == 0) {
            size_t pn = strlen(v);
            if (pn >= sizeof(m->path)) {
                fprintf(stderr, "vm.cfg: line %u: [mount.%s] path too long\n",
                        cfg->entries[i].line, name);
                return false;
            }
            memcpy(m->path, v, pn + 1);
        } else if (strcmp(k, "writable") == 0) {
            if (inicfg_get_bool(cfg, s, k, &bv)) {
                m->writable = bv;
            } else {
                fprintf(stderr, "vm.cfg: line %u: [mount.%s] writable: "
                        "unparseable bool '%s'\n",
                        cfg->entries[i].line, name, v);
                return false;
            }
        } else if (strcmp(k, "size_kb") == 0) {
            if (inicfg_get_int(cfg, s, k, &lv)) {
                if (lv < 8 || lv > 4096) {
                    fprintf(stderr, "vm.cfg: line %u: [mount.%s] "
                            "size_kb=%ld out of range (8..4096)\n",
                            cfg->entries[i].line, name, lv);
                    return false;
                }
                m->size_kb = (uint32_t)lv;
            } else {
                fprintf(stderr, "vm.cfg: line %u: [mount.%s] size_kb: "
                        "unparseable integer '%s'\n",
                        cfg->entries[i].line, name, v);
                return false;
            }
        } else {
            fprintf(stderr, "vm.cfg: line %u: warning: unknown key "
                    "'%s.%s'\n", cfg->entries[i].line, s, k);
        }
    }

    /* Warn on unknown keys so typos surface. We allow unknown sections
     * (e.g., a future [scheduler]) so older hosts don't reject newer
     * configs. [mount.<name>] sections are already handled above. */
    static const struct {
        const char *section;
        const char *keys[8];     /* NULL-terminated */
    } known[] = {
        { "memory", {"local_kb", "shared_kb", "max_vms",
                     "spawn_data_kb", NULL} },
        { "stdio",  {"raw_mode", NULL} },
    };
    for (size_t i = 0; i < cfg->count; i++) {
        const char *s = cfg->entries[i].section;
        const char *k = cfg->entries[i].key;
        if (strncmp(s, "mount.", 6) == 0) continue;
        bool found_section = false;
        bool found_key = false;
        for (size_t j = 0; j < sizeof(known) / sizeof(known[0]); j++) {
            if (strcmp(s, known[j].section) != 0) continue;
            found_section = true;
            for (size_t m = 0; known[j].keys[m]; m++) {
                if (strcmp(k, known[j].keys[m]) == 0) {
                    found_key = true;
                    break;
                }
            }
            break;
        }
        if (found_section && !found_key) {
            fprintf(stderr, "vm.cfg: line %u: warning: unknown key "
                    "'%s.%s'\n", cfg->entries[i].line, s, k);
        }
        /* Unknown section: silent, future-compat. */
    }
    return true;
}

bool host_config_load(const char *path, HostConfig *hc) {
    bool explicit_path = (path != NULL);
    if (!path) path = "vm.cfg";

    /* If the path isn't explicit, peek to see if the default config
     * exists. Missing default is fine; we just keep built-in defaults. */
    if (!explicit_path) {
        FILE *probe = fopen(path, "rb");
        if (!probe) return true;
        fclose(probe);
    }

    IniCfg cfg;
    IniCfgError err;
    if (!inicfg_parse_file(path, &cfg, &err)) {
        if (err.line == 0) {
            fprintf(stderr, "host: %s\n", err.msg);
        } else {
            fprintf(stderr, "host: %s: line %u: %s\n",
                    path, err.line, err.msg);
        }
        return false;
    }
    bool ok = apply_inicfg(&cfg, hc);
    inicfg_destroy(&cfg);
    return ok;
}
