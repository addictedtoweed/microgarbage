/* ============================================================
 *  test_runner.h — minimal copy-paste test framework
 *
 *  Header-only. Drop into any C project, #include from a test
 *  file, write `static void test_foo(void) { ... }` for each
 *  test, list them in main():
 *
 *      int main(void) {
 *          TEST_SUITE("my_module");
 *          RUN(test_foo);
 *          RUN(test_bar);
 *          return TEST_SUITE_RESULT();
 *      }
 *
 *  Each test prints PASS or FAIL on its own line. Failure
 *  messages include file:line and the actual vs. expected values.
 *  Exit code is 0 if all passed, 1 if any failed — pipe-friendly
 *  for shell test runners.
 *
 *  Tests use ASSERT_* macros which print a failure message and
 *  return from the test function on first failure. The runner
 *  keeps going to the next test, so one bad assertion doesn't
 *  hide the rest of the report.
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef TEST_RUNNER_H
#define TEST_RUNNER_H

#include <stdio.h>
#include <string.h>

/* Module-local bookkeeping. Each test file gets its own copy.
 * Declared `static` to avoid ODR violations when multiple test
 * files are linked together (which we don't do, but worth being
 * tidy). */
static int  tr_passed_   = 0;
static int  tr_failed_   = 0;
static int  tr_current_failed_ = 0;   /* did the running test fail yet */
static const char *tr_suite_name_ = "(unnamed)";
static const char *tr_current_test_name_ = "(unset)";

/* Begin a suite. Resets counters and prints a header. Call once
 * at the start of main(). */
#define TEST_SUITE(name) do { \
    tr_suite_name_ = (name); \
    tr_passed_ = tr_failed_ = 0; \
    printf("=== %s ===\n", tr_suite_name_); \
} while (0)

/* Run a single test function. Records its name (for failure
 * messages) and reports the result. */
#define RUN(fn) do { \
    tr_current_failed_ = 0; \
    tr_current_test_name_ = #fn; \
    fn(); \
    if (tr_current_failed_) { \
        tr_failed_++; \
    } else { \
        tr_passed_++; \
        printf("  PASS  %s\n", #fn); \
    } \
} while (0)

/* Print summary, return exit code. Use as the return value
 * of main(). */
#define TEST_SUITE_RESULT() ( \
    printf("%d passed, %d failed\n", tr_passed_, tr_failed_), \
    tr_failed_ == 0 ? 0 : 1 \
)

/* Internal: print a failure and mark the current test failed.
 * Subsequent ASSERT_* in the same test won't print (the macros
 * return on first failure), but the guard is defensive. */
#define TR_FAIL_AT(file, line, ...) do { \
    if (!tr_current_failed_) { \
        printf("  FAIL  %s  at %s:%d: ", \
               tr_current_test_name_, (file), (line)); \
        printf(__VA_ARGS__); \
        putchar('\n'); \
        tr_current_failed_ = 1; \
    } \
} while (0)

/* Assertions. Each one prints a failure and returns on mismatch. */

#define ASSERT(cond) do { \
    if (!(cond)) { \
        TR_FAIL_AT(__FILE__, __LINE__, "assertion failed: %s", #cond); \
        return; \
    } \
} while (0)

#define ASSERT_EQ_INT(expected, actual) do { \
    long long _e = (long long)(expected); \
    long long _a = (long long)(actual); \
    if (_e != _a) { \
        TR_FAIL_AT(__FILE__, __LINE__, \
            "expected %lld, got %lld  (%s vs %s)", \
            _e, _a, #expected, #actual); \
        return; \
    } \
} while (0)

#define ASSERT_EQ_PTR(expected, actual) do { \
    const void *_e = (const void*)(expected); \
    const void *_a = (const void*)(actual); \
    if (_e != _a) { \
        TR_FAIL_AT(__FILE__, __LINE__, \
            "expected pointer %p, got %p  (%s vs %s)", \
            _e, _a, #expected, #actual); \
        return; \
    } \
} while (0)

#define ASSERT_EQ_STR(expected, actual) do { \
    const char *_e = (expected); \
    const char *_a = (actual); \
    if (_e == NULL || _a == NULL || strcmp(_e, _a) != 0) { \
        TR_FAIL_AT(__FILE__, __LINE__, \
            "expected \"%s\", got \"%s\"", \
            _e ? _e : "(null)", _a ? _a : "(null)"); \
        return; \
    } \
} while (0)

#define ASSERT_NULL(p) do { \
    const void *_p = (const void*)(p); \
    if (_p != NULL) { \
        TR_FAIL_AT(__FILE__, __LINE__, \
            "expected NULL, got %p  (%s)", _p, #p); \
        return; \
    } \
} while (0)

#define ASSERT_NOT_NULL(p) do { \
    const void *_p = (const void*)(p); \
    if (_p == NULL) { \
        TR_FAIL_AT(__FILE__, __LINE__, \
            "expected non-NULL  (%s)", #p); \
        return; \
    } \
} while (0)

#define FAIL(...) do { \
    TR_FAIL_AT(__FILE__, __LINE__, __VA_ARGS__); \
    return; \
} while (0)

#endif /* TEST_RUNNER_H */
