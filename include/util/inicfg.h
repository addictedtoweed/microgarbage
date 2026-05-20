/* ============================================================
 *  inicfg.h — minimal INI-style config parser
 *
 *  A handwritten, dependency-free INI reader for host-side use.
 *  Designed for the microgarbage host's vm.cfg, but completely
 *  general.
 *
 *  ---------------------------------------------------------------
 *  Format
 *  ---------------------------------------------------------------
 *
 *      # comments start with # or ;
 *      ; like this
 *
 *      [section]              # square-bracketed section header
 *      key = value            # whitespace around '=' is ignored
 *      key.with.dots = value  # dots are legal in keys
 *      empty_value =          # empty values are allowed
 *
 *      [other.section]        # dots in section names are also fine
 *      x = 42
 *
 *  Rules:
 *
 *    - Section and key names are case-sensitive ASCII. The
 *      parser does not lower-case anything. Pick a convention
 *      and stick to it; we use lowercase.snake_case.
 *
 *    - Values are stored verbatim (after trimming leading/trailing
 *      whitespace). Quoting is not supported; if you need spaces
 *      in a value, just put them there — the parser keeps everything
 *      from the first non-whitespace character after '=' to the
 *      last non-whitespace character before EOL.
 *
 *    - Lines without an '=' (and without a section header) are
 *      a parse error — explicit > silent fall-through.
 *
 *    - Lines outside any section land under the empty-string
 *      section "". Up to your binder code whether to allow this.
 *
 *    - There is no include/import directive, no variable
 *      substitution, no escape sequences. If you need any of
 *      those, this isn't the format.
 *
 *  ---------------------------------------------------------------
 *  Usage
 *  ---------------------------------------------------------------
 *
 *      IniCfg cfg;
 *      IniCfgError err;
 *      if (!inicfg_parse_file("vm.cfg", &cfg, &err)) {
 *          fprintf(stderr, "vm.cfg: line %u: %s\n", err.line, err.msg);
 *          return 1;
 *      }
 *
 *      const char *v = inicfg_get(&cfg, "memory", "local_kb");
 *      if (v) { ... atoi(v) etc. ... }
 *
 *      // Iterate everything (for unknown-key warnings, dumps, etc):
 *      for (size_t i = 0; i < cfg.count; i++) {
 *          printf("[%s] %s = %s\n",
 *              cfg.entries[i].section,
 *              cfg.entries[i].key,
 *              cfg.entries[i].value);
 *      }
 *
 *      inicfg_destroy(&cfg);
 *
 *  ---------------------------------------------------------------
 *  Memory model
 *  ---------------------------------------------------------------
 *
 *  inicfg_parse_file mallocs both the entries array and the
 *  string storage. inicfg_destroy frees everything. The strings
 *  pointed to by `section`, `key`, and `value` live inside the
 *  parser-owned arena, so they are stable until destroy and
 *  must not be freed individually.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef MICROGARBAGE_INICFG_H
#define MICROGARBAGE_INICFG_H

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *section;   /* may be "" for entries before any [section] */
    const char *key;
    const char *value;     /* may be "" but never NULL */
    unsigned    line;      /* 1-based source line, for error reporting */
} IniCfgEntry;

typedef struct {
    IniCfgEntry *entries;
    size_t       count;

    /* Internal: backing storage for all entry strings, freed by
     * inicfg_destroy. Don't poke at it. */
    char        *_arena;
    size_t       _arena_len;
} IniCfg;

typedef struct {
    unsigned line;          /* 1-based line where the error occurred */
    char     msg[128];      /* human-readable; "" on success */
} IniCfgError;

/* Parse a file at `path` into `out`. On success, returns true
 * and fills `out`. On failure (file not found, parse error,
 * out-of-memory), returns false; if `err` is non-NULL, fills it
 * with details. `out` is left in a safe-to-destroy state either
 * way. */
bool inicfg_parse_file(const char *path, IniCfg *out, IniCfgError *err);

/* Parse a null-terminated in-memory string. Useful for tests
 * and for hosts that ship a default config inline. The string
 * is copied; the caller can free `text` immediately after. */
bool inicfg_parse_string(const char *text, IniCfg *out, IniCfgError *err);

/* Lookup the first entry matching (section, key). Returns the
 * value string, or NULL if not found. Case-sensitive. */
const char *inicfg_get(const IniCfg *cfg,
                       const char *section, const char *key);

/* Same as inicfg_get but parses the value as a signed integer.
 * Accepts decimal (e.g., "42") and 0x-prefixed hex (e.g., "0x1F").
 * Returns true on success, false if the key is missing OR the
 * value can't be parsed. `*out` is left untouched on failure. */
bool inicfg_get_int(const IniCfg *cfg,
                    const char *section, const char *key,
                    long *out);

/* Parse a boolean. Recognizes (case-insensitive):
 *   true / yes / on / 1   -> true
 *   false / no / off / 0  -> false
 * Returns true on success, false if missing or unparseable. */
bool inicfg_get_bool(const IniCfg *cfg,
                     const char *section, const char *key,
                     bool *out);

/* Release all storage owned by `cfg`. Safe to call on a
 * zero-initialized or already-destroyed IniCfg. */
void inicfg_destroy(IniCfg *cfg);

#ifdef __cplusplus
}
#endif

#endif /* MICROGARBAGE_INICFG_H */
