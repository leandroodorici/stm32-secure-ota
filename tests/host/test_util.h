#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#include <stdio.h>

extern int g_tests_run;
extern int g_tests_failed;

// A failed check returns from the current test rather than aborting, so one
// broken assumption does not hide every later result.
#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                     \
            g_tests_failed++;                                                            \
            return;                                                                      \
        }                                                                                \
    } while (0)

#define CHECK_EQ(a, b)                                                                   \
    do {                                                                                 \
        long long _a = (long long)(a);                                                   \
        long long _b = (long long)(b);                                                   \
        if (_a != _b) {                                                                  \
            printf("  FAIL %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a,    \
                   #b, _a, _b);                                                          \
            g_tests_failed++;                                                            \
            return;                                                                      \
        }                                                                                \
    } while (0)

#define RUN(fn)                                                                          \
    do {                                                                                 \
        int before = g_tests_failed;                                                     \
        g_tests_run++;                                                                   \
        fn();                                                                            \
        if (g_tests_failed == before) {                                                  \
            printf("  ok   %s\n", #fn);                                                  \
        }                                                                                \
    } while (0)

#define TEST_MAIN_BEGIN(suite)                                                           \
    int g_tests_run = 0;                                                                 \
    int g_tests_failed = 0;                                                              \
    int main(void)                                                                       \
    {                                                                                    \
        printf("== %s ==\n", suite);

// Non-zero exit on failure is what ctest and CI key off.
#define TEST_MAIN_END()                                                                  \
    printf("%d run, %d failed\n", g_tests_run, g_tests_failed);                          \
    return g_tests_failed ? 1 : 0;                                                       \
    }

#endif // TEST_UTIL_H