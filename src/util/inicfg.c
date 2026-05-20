/* ============================================================
 *  inicfg.c — minimal INI-style config parser
 *  See util/inicfg.h for the public contract.
 *
 *  Implementation notes
 *  --------------------
 *
 *  One-pass parser. We make a single mutable copy of the input
 *  in `cfg->_arena` and then chop it into null-terminated runs
 *  in place. Entry strings point into the arena, so freeing
 *  the IniCfg releases everything at once.
 *
 *  Entry storage uses geometric growth (doubling) so we don't
 *  need a pre-pass to count entries. Worst case wastes < 2x the
 *  needed slots, which for a 200-line config is < 1 KB.
 *
 *  No external dependencies beyond libc.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "util/inicfg.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 *  Internal helpers
 * ============================================================ */

static void set_err(IniCfgError *err, unsigned line, const char *msg) {
    if (!err) return;
    err->line = line;
    /* snprintf is safe with NULL fmt arg in some libcs but not all;
     * make the contract explicit. */
    if (!msg) msg = "(no message)";
    size_t cap = sizeof(err->msg);
    size_t i = 0;
    while (i + 1 < cap && msg[i] != '\0') {
        err->msg[i] = msg[i];
        i++;
    }
    err->msg[i] = '\0';
}

/* In-place trim: returns a pointer to the first non-whitespace
 * character and null-terminates after the last non-whitespace
 * character. Empty input returns a pointer to the trailing null. */
static char *trim_inplace(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n')) {
        end--;
    }
    *end = '\0';
    return s;
}

/* Grow `entries` to hold at least `min_cap` items. Returns NULL
 * on OOM. */
static IniCfgEntry *grow_entries(IniCfgEntry *entries, size_t *cap,
                                 size_t min_cap) {
    size_t new_cap = (*cap == 0) ? 8 : *cap;
    while (new_cap < min_cap) {
        size_t n = new_cap * 2;
        if (n <= new_cap) return NULL;   /* overflow */
        new_cap = n;
    }
    IniCfgEntry *re = (IniCfgEntry *)realloc(entries,
                                              new_cap * sizeof(*re));
    if (!re) return NULL;
    *cap = new_cap;
    return re;
}

/* Case-insensitive ASCII compare. */
static int strieq_ascii(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/* ============================================================
 *  Parsing
 * ============================================================ */

bool inicfg_parse_string(const char *text, IniCfg *out, IniCfgError *err) {
    if (!out) {
        set_err(err, 0, "out is NULL");
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (err) { err->line = 0; err->msg[0] = '\0'; }
    if (!text) text = "";

    /* Copy the input into our arena. The trailing null is included
     * so we can do strlen-style scanning. */
    size_t text_len = strlen(text);
    out->_arena = (char *)malloc(text_len + 1);
    if (!out->_arena) {
        set_err(err, 0, "out of memory");
        return false;
    }
    memcpy(out->_arena, text, text_len + 1);
    out->_arena_len = text_len + 1;

    IniCfgEntry *entries = NULL;
    size_t cap = 0;
    size_t count = 0;

    /* "" is a valid section name (entries before any [section]). */
    const char *current_section = "";

    /* Walk the arena line-by-line. We scan for '\n' and replace
     * it with '\0' so each line is a standalone null-terminated
     * substring we can pass to trim. */
    char *p = out->_arena;
    char *end = out->_arena + text_len;
    unsigned lineno = 0;

    while (p < end) {
        lineno++;
        /* Find end of line. */
        char *line = p;
        while (p < end && *p != '\n') p++;
        if (p < end) {
            *p = '\0';
            p++;   /* advance past the now-null */
        }

        char *t = trim_inplace(line);

        /* Skip blank lines and comments. */
        if (*t == '\0' || *t == '#' || *t == ';') continue;

        /* Section header: [section.name] */
        if (*t == '[') {
            char *rb = strchr(t, ']');
            if (!rb) {
                set_err(err, lineno, "section header missing ']'");
                free(entries);
                inicfg_destroy(out);
                return false;
            }
            *rb = '\0';
            current_section = trim_inplace(t + 1);
            if (*current_section == '\0') {
                set_err(err, lineno, "empty section name");
                free(entries);
                inicfg_destroy(out);
                return false;
            }
            continue;
        }

        /* key = value */
        char *eq = strchr(t, '=');
        if (!eq) {
            set_err(err, lineno, "expected 'key = value'");
            free(entries);
            inicfg_destroy(out);
            return false;
        }
        *eq = '\0';
        char *key = trim_inplace(t);
        char *value = trim_inplace(eq + 1);
        if (*key == '\0') {
            set_err(err, lineno, "empty key");
            free(entries);
            inicfg_destroy(out);
            return false;
        }

        /* Strip inline comments from the value. We do this AFTER
         * the trim so a '#' inside whitespace at end-of-line is
         * already gone; this catches the common case "key = 42 # note".
         * A '#' in the middle of a value is preserved if there's no
         * preceding space — i.e., "url = http://x#fragment" works,
         * but "url = http://x #fragment" does not. */
        for (char *c = value; *c; c++) {
            if ((*c == '#' || *c == ';') &&
                c > value && (c[-1] == ' ' || c[-1] == '\t')) {
                *c = '\0';
                value = trim_inplace(value);
                break;
            }
        }

        if (count + 1 > cap) {
            IniCfgEntry *re = grow_entries(entries, &cap, count + 1);
            if (!re) {
                set_err(err, lineno, "out of memory");
                free(entries);
                inicfg_destroy(out);
                return false;
            }
            entries = re;
        }
        entries[count].section = current_section;
        entries[count].key     = key;
        entries[count].value   = value;
        entries[count].line    = lineno;
        count++;
    }

    out->entries = entries;
    out->count   = count;
    return true;
}

bool inicfg_parse_file(const char *path, IniCfg *out, IniCfgError *err) {
    if (!out) {
        set_err(err, 0, "out is NULL");
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (err) { err->line = 0; err->msg[0] = '\0'; }
    if (!path) {
        set_err(err, 0, "path is NULL");
        return false;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        char buf[128];
        snprintf(buf, sizeof(buf), "cannot open '%s'", path);
        set_err(err, 0, buf);
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        set_err(err, 0, "fseek failed");
        return false;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        set_err(err, 0, "ftell failed");
        return false;
    }
    rewind(f);

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        set_err(err, 0, "out of memory");
        return false;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) {
        free(buf);
        set_err(err, 0, "fread short");
        return false;
    }
    buf[sz] = '\0';

    bool ok = inicfg_parse_string(buf, out, err);
    free(buf);
    return ok;
}

/* ============================================================
 *  Lookup
 * ============================================================ */

const char *inicfg_get(const IniCfg *cfg,
                       const char *section, const char *key) {
    if (!cfg || !section || !key) return NULL;
    for (size_t i = 0; i < cfg->count; i++) {
        if (strcmp(cfg->entries[i].section, section) == 0 &&
            strcmp(cfg->entries[i].key, key) == 0) {
            return cfg->entries[i].value;
        }
    }
    return NULL;
}

bool inicfg_get_int(const IniCfg *cfg,
                    const char *section, const char *key,
                    long *out) {
    const char *v = inicfg_get(cfg, section, key);
    if (!v || !out) return false;
    /* Skip leading whitespace (shouldn't be any after parse, but defensive). */
    while (*v == ' ' || *v == '\t') v++;
    if (*v == '\0') return false;

    char *endp = NULL;
    /* strtol auto-handles 0x prefix when base=0. Negative numbers OK. */
    long val = strtol(v, &endp, 0);
    if (endp == v) return false;        /* no digits parsed */
    /* Allow trailing whitespace but nothing else. */
    while (*endp == ' ' || *endp == '\t') endp++;
    if (*endp != '\0') return false;
    *out = val;
    return true;
}

bool inicfg_get_bool(const IniCfg *cfg,
                     const char *section, const char *key,
                     bool *out) {
    const char *v = inicfg_get(cfg, section, key);
    if (!v || !out) return false;
    if (strieq_ascii(v, "true")  || strieq_ascii(v, "yes") ||
        strieq_ascii(v, "on")    || strcmp(v, "1") == 0) {
        *out = true;  return true;
    }
    if (strieq_ascii(v, "false") || strieq_ascii(v, "no") ||
        strieq_ascii(v, "off")   || strcmp(v, "0") == 0) {
        *out = false; return true;
    }
    return false;
}

/* ============================================================
 *  Cleanup
 * ============================================================ */

void inicfg_destroy(IniCfg *cfg) {
    if (!cfg) return;
    free(cfg->entries);
    free(cfg->_arena);
    cfg->entries = NULL;
    cfg->_arena  = NULL;
    cfg->count   = 0;
    cfg->_arena_len = 0;
}
