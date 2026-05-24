/* ============================================================
 *  test_inicfg.c — tests for the INI parser.
 *
 *  These exercise the public API only. The parser is small
 *  enough that we don't need to peek at internals.
 * ============================================================ */

#include "util/inicfg.h"
#include "test_runner.h"
#include "test_portable.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------
 *  Basic happy path
 * --------------------------------------------------------------- */

static void test_parse_empty(void) {
    IniCfg cfg;
    ASSERT(inicfg_parse_string("", &cfg, NULL));
    ASSERT_EQ_INT(0, (int)cfg.count);
    inicfg_destroy(&cfg);
}

static void test_parse_single_key(void) {
    IniCfg cfg;
    ASSERT(inicfg_parse_string("foo = 42", &cfg, NULL));
    ASSERT_EQ_INT(1, (int)cfg.count);
    ASSERT_EQ_STR("",    cfg.entries[0].section);
    ASSERT_EQ_STR("foo", cfg.entries[0].key);
    ASSERT_EQ_STR("42",  cfg.entries[0].value);
    inicfg_destroy(&cfg);
}

static void test_parse_section_and_keys(void) {
    const char *text =
        "[memory]\n"
        "local_kb = 1536\n"
        "max_vms = 4\n";
    IniCfg cfg;
    ASSERT(inicfg_parse_string(text, &cfg, NULL));
    ASSERT_EQ_INT(2, (int)cfg.count);
    ASSERT_EQ_STR("memory",   cfg.entries[0].section);
    ASSERT_EQ_STR("local_kb", cfg.entries[0].key);
    ASSERT_EQ_STR("1536",     cfg.entries[0].value);
    ASSERT_EQ_STR("memory",   cfg.entries[1].section);
    ASSERT_EQ_STR("max_vms",  cfg.entries[1].key);
    ASSERT_EQ_STR("4",        cfg.entries[1].value);
    inicfg_destroy(&cfg);
}

static void test_parse_dotted_section_and_key(void) {
    const char *text =
        "[memory.bins]\n"
        "five.point.zero = 1\n";
    IniCfg cfg;
    ASSERT(inicfg_parse_string(text, &cfg, NULL));
    ASSERT_EQ_INT(1, (int)cfg.count);
    ASSERT_EQ_STR("memory.bins",     cfg.entries[0].section);
    ASSERT_EQ_STR("five.point.zero", cfg.entries[0].key);
    inicfg_destroy(&cfg);
}

static void test_parse_blank_lines_and_comments(void) {
    const char *text =
        "# top comment\n"
        "; also a comment\n"
        "\n"
        "[memory]\n"
        "  \n"           /* whitespace-only */
        "  local_kb = 1024  # inline comment\n"
        "  max_vms  = 2 ; another\n"
        "\n";
    IniCfg cfg;
    ASSERT(inicfg_parse_string(text, &cfg, NULL));
    ASSERT_EQ_INT(2, (int)cfg.count);
    ASSERT_EQ_STR("local_kb", cfg.entries[0].key);
    ASSERT_EQ_STR("1024",     cfg.entries[0].value);
    ASSERT_EQ_STR("max_vms",  cfg.entries[1].key);
    ASSERT_EQ_STR("2",        cfg.entries[1].value);
    inicfg_destroy(&cfg);
}

static void test_parse_empty_value_is_allowed(void) {
    IniCfg cfg;
    ASSERT(inicfg_parse_string("foo =\n", &cfg, NULL));
    ASSERT_EQ_INT(1, (int)cfg.count);
    ASSERT_EQ_STR("foo", cfg.entries[0].key);
    ASSERT_EQ_STR("",    cfg.entries[0].value);
    inicfg_destroy(&cfg);
}

static void test_parse_value_with_spaces(void) {
    IniCfg cfg;
    ASSERT(inicfg_parse_string("greeting = hello world\n", &cfg, NULL));
    ASSERT_EQ_INT(1, (int)cfg.count);
    ASSERT_EQ_STR("hello world", cfg.entries[0].value);
    inicfg_destroy(&cfg);
}

static void test_parse_url_with_hash_is_preserved(void) {
    /* Inline comments require whitespace before # / ; — a
     * tight token like a URL fragment must survive intact. */
    IniCfg cfg;
    ASSERT(inicfg_parse_string("url = http://x#fragment\n", &cfg, NULL));
    ASSERT_EQ_STR("http://x#fragment", cfg.entries[0].value);
    inicfg_destroy(&cfg);
}

static void test_parse_crlf_line_endings(void) {
    IniCfg cfg;
    const char *text = "[a]\r\nfoo = bar\r\n";
    ASSERT(inicfg_parse_string(text, &cfg, NULL));
    ASSERT_EQ_INT(1, (int)cfg.count);
    ASSERT_EQ_STR("a",   cfg.entries[0].section);
    ASSERT_EQ_STR("foo", cfg.entries[0].key);
    ASSERT_EQ_STR("bar", cfg.entries[0].value);
    inicfg_destroy(&cfg);
}

/* ---------------------------------------------------------------
 *  Error paths
 * --------------------------------------------------------------- */

static void test_parse_unterminated_section(void) {
    IniCfg cfg;
    IniCfgError err;
    ASSERT(!inicfg_parse_string("[memory\nfoo = 1\n", &cfg, &err));
    ASSERT_EQ_INT(1, (int)err.line);
    inicfg_destroy(&cfg);
}

static void test_parse_empty_section_name(void) {
    IniCfg cfg;
    IniCfgError err;
    ASSERT(!inicfg_parse_string("[]\nfoo = 1\n", &cfg, &err));
    ASSERT_EQ_INT(1, (int)err.line);
    inicfg_destroy(&cfg);
}

static void test_parse_missing_equals(void) {
    IniCfg cfg;
    IniCfgError err;
    ASSERT(!inicfg_parse_string("foo bar\n", &cfg, &err));
    ASSERT_EQ_INT(1, (int)err.line);
    inicfg_destroy(&cfg);
}

static void test_parse_empty_key(void) {
    IniCfg cfg;
    IniCfgError err;
    ASSERT(!inicfg_parse_string("= 42\n", &cfg, &err));
    ASSERT_EQ_INT(1, (int)err.line);
    inicfg_destroy(&cfg);
}

static void test_parse_error_line_number(void) {
    /* Error on line 4: should be reported as line 4 even if
     * earlier lines were comments/blank. */
    IniCfg cfg;
    IniCfgError err;
    const char *text =
        "# line 1\n"
        "\n"
        "[ok]\n"
        "bad line no equals\n";
    ASSERT(!inicfg_parse_string(text, &cfg, &err));
    ASSERT_EQ_INT(4, (int)err.line);
    inicfg_destroy(&cfg);
}

/* ---------------------------------------------------------------
 *  Lookup API
 * --------------------------------------------------------------- */

static void test_get_finds_existing(void) {
    IniCfg cfg;
    ASSERT(inicfg_parse_string("[s]\nk = v\n", &cfg, NULL));
    const char *v = inicfg_get(&cfg, "s", "k");
    ASSERT_NOT_NULL(v);
    ASSERT_EQ_STR("v", v);
    inicfg_destroy(&cfg);
}

static void test_get_returns_null_for_missing(void) {
    IniCfg cfg;
    ASSERT(inicfg_parse_string("[s]\nk = v\n", &cfg, NULL));
    ASSERT_NULL(inicfg_get(&cfg, "s", "missing"));
    ASSERT_NULL(inicfg_get(&cfg, "other", "k"));
    inicfg_destroy(&cfg);
}

static void test_get_int_decimal(void) {
    IniCfg cfg;
    ASSERT(inicfg_parse_string("[a]\nn = 42\n", &cfg, NULL));
    long n = 0;
    ASSERT(inicfg_get_int(&cfg, "a", "n", &n));
    ASSERT_EQ_INT(42, (int)n);
    inicfg_destroy(&cfg);
}

static void test_get_int_hex(void) {
    IniCfg cfg;
    ASSERT(inicfg_parse_string("[a]\nn = 0x1F\n", &cfg, NULL));
    long n = 0;
    ASSERT(inicfg_get_int(&cfg, "a", "n", &n));
    ASSERT_EQ_INT(31, (int)n);
    inicfg_destroy(&cfg);
}

static void test_get_int_negative(void) {
    IniCfg cfg;
    ASSERT(inicfg_parse_string("[a]\nn = -17\n", &cfg, NULL));
    long n = 0;
    ASSERT(inicfg_get_int(&cfg, "a", "n", &n));
    ASSERT_EQ_INT(-17, (int)n);
    inicfg_destroy(&cfg);
}

static void test_get_int_rejects_garbage(void) {
    IniCfg cfg;
    ASSERT(inicfg_parse_string("[a]\nn = 42xyz\n", &cfg, NULL));
    long n = 999;
    ASSERT(!inicfg_get_int(&cfg, "a", "n", &n));
    ASSERT_EQ_INT(999, (int)n);  /* untouched on failure */
    inicfg_destroy(&cfg);
}

static void test_get_bool_variants(void) {
    IniCfg cfg;
    const char *text =
        "[b]\n"
        "t1 = true\n"
        "t2 = YES\n"
        "t3 = on\n"
        "t4 = 1\n"
        "f1 = false\n"
        "f2 = No\n"
        "f3 = off\n"
        "f4 = 0\n";
    ASSERT(inicfg_parse_string(text, &cfg, NULL));
    bool b = false;
    ASSERT(inicfg_get_bool(&cfg, "b", "t1", &b)); ASSERT(b);
    ASSERT(inicfg_get_bool(&cfg, "b", "t2", &b)); ASSERT(b);
    ASSERT(inicfg_get_bool(&cfg, "b", "t3", &b)); ASSERT(b);
    ASSERT(inicfg_get_bool(&cfg, "b", "t4", &b)); ASSERT(b);
    ASSERT(inicfg_get_bool(&cfg, "b", "f1", &b)); ASSERT(!b);
    ASSERT(inicfg_get_bool(&cfg, "b", "f2", &b)); ASSERT(!b);
    ASSERT(inicfg_get_bool(&cfg, "b", "f3", &b)); ASSERT(!b);
    ASSERT(inicfg_get_bool(&cfg, "b", "f4", &b)); ASSERT(!b);
    inicfg_destroy(&cfg);
}

static void test_get_bool_rejects_garbage(void) {
    IniCfg cfg;
    ASSERT(inicfg_parse_string("[b]\nx = sometimes\n", &cfg, NULL));
    bool b = true;
    ASSERT(!inicfg_get_bool(&cfg, "b", "x", &b));
    ASSERT(b);   /* untouched on failure */
    inicfg_destroy(&cfg);
}

/* ---------------------------------------------------------------
 *  File-based parse
 * --------------------------------------------------------------- */

static void test_parse_file_roundtrip(void) {
    /* Write a temp file, parse it back, verify. */
    char pathbuf[256];
    const char *path = tp_path(pathbuf, sizeof pathbuf, "inicfg_test_001.ini");
    FILE *f = fopen(path, "wb");
    ASSERT_NOT_NULL(f);
    fputs("[mem]\nkb = 64\n", f);
    fclose(f);

    IniCfg cfg;
    IniCfgError err;
    ASSERT(inicfg_parse_file(path, &cfg, &err));
    ASSERT_EQ_INT(1, (int)cfg.count);
    ASSERT_EQ_STR("mem", cfg.entries[0].section);
    ASSERT_EQ_STR("kb",  cfg.entries[0].key);
    ASSERT_EQ_STR("64",  cfg.entries[0].value);
    inicfg_destroy(&cfg);
    remove(path);
}

static void test_parse_file_missing_reports_error(void) {
    IniCfg cfg;
    IniCfgError err;
    char nbuf[256];
    ASSERT(!inicfg_parse_file(tp_path(nbuf, sizeof nbuf,
                                      "inicfg_test_does_not_exist.ini"),
                              &cfg, &err));
    /* Doesn't matter what the message says exactly, just that
     * line==0 (it's a file-level error, not a parse error). */
    ASSERT_EQ_INT(0, (int)err.line);
    inicfg_destroy(&cfg);
}

/* ---------------------------------------------------------------
 *  Lifecycle
 * --------------------------------------------------------------- */

static void test_destroy_is_safe_on_zero_init(void) {
    IniCfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    inicfg_destroy(&cfg);   /* must not crash */
    inicfg_destroy(&cfg);   /* idempotent */
}

static void test_destroy_after_failed_parse_is_safe(void) {
    IniCfg cfg;
    IniCfgError err;
    ASSERT(!inicfg_parse_string("oops no equals\n", &cfg, &err));
    inicfg_destroy(&cfg);
}

int main(void) {
    TEST_SUITE("inicfg");

    /* Happy paths */
    RUN(test_parse_empty);
    RUN(test_parse_single_key);
    RUN(test_parse_section_and_keys);
    RUN(test_parse_dotted_section_and_key);
    RUN(test_parse_blank_lines_and_comments);
    RUN(test_parse_empty_value_is_allowed);
    RUN(test_parse_value_with_spaces);
    RUN(test_parse_url_with_hash_is_preserved);
    RUN(test_parse_crlf_line_endings);

    /* Errors */
    RUN(test_parse_unterminated_section);
    RUN(test_parse_empty_section_name);
    RUN(test_parse_missing_equals);
    RUN(test_parse_empty_key);
    RUN(test_parse_error_line_number);

    /* Lookups */
    RUN(test_get_finds_existing);
    RUN(test_get_returns_null_for_missing);
    RUN(test_get_int_decimal);
    RUN(test_get_int_hex);
    RUN(test_get_int_negative);
    RUN(test_get_int_rejects_garbage);
    RUN(test_get_bool_variants);
    RUN(test_get_bool_rejects_garbage);

    /* File */
    RUN(test_parse_file_roundtrip);
    RUN(test_parse_file_missing_reports_error);

    /* Lifecycle */
    RUN(test_destroy_is_safe_on_zero_init);
    RUN(test_destroy_after_failed_parse_is_safe);

    return TEST_SUITE_RESULT();
}
