/*
 * Minimal test harness for Arbiter's C tests (host and QNX). No third-party code.
 *
 * Each test is a static void function named after the requirement it covers,
 * e.g. fr_g4_joint_limit_exactly_at_max. A test file lists its tests in a
 * static table and calls arb_test_run(). Output, one line per test:
 *   PASS <name>   or   FAIL <name>   (with the failing checks above it)
 * then "RESULT PASS|FAIL <suite> <passed>/<total>". Exit status 0 = all passed.
 * scripts/check-traceability.py looks up test names in these files.
 */
#ifndef ARB_TEST_H
#define ARB_TEST_H

#include <stdio.h>

typedef struct {
    const char *name;
    void (*fn)(void);
} arb_test_case_t;

static int arb_test_failures;

#define ARB_TEST_CASE(fn) { #fn, fn }

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            arb_test_failures++;                                                      \
            printf("  check failed: %s  (%s:%d)\n", #cond, __FILE__, __LINE__);       \
        }                                                                             \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                            \
    do {                                                                              \
        long long arb_a_ = (long long)(a), arb_b_ = (long long)(b);                   \
        if (arb_a_ != arb_b_) {                                                       \
            arb_test_failures++;                                                      \
            printf("  check failed: %s == %s  (%lld != %lld)  (%s:%d)\n",            \
                   #a, #b, arb_a_, arb_b_, __FILE__, __LINE__);                       \
        }                                                                             \
    } while (0)

static int arb_test_run(const char *suite, const arb_test_case_t *cases, int n)
{
    int passed = 0;

    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int i = 0; i < n; i++) {                 /* bound: n, the table size */
        const int before = arb_test_failures;
        cases[i].fn();
        if (arb_test_failures == before) {
            passed++;
            printf("PASS %s\n", cases[i].name);
        } else {
            printf("FAIL %s\n", cases[i].name);
        }
    }
    printf("RESULT %s %s %d/%d\n", passed == n ? "PASS" : "FAIL", suite, passed, n);
    return passed == n ? 0 : 1;
}

#endif /* ARB_TEST_H */
