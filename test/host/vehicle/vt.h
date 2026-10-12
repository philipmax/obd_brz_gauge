#pragma once
/* Minimal assertion harness for the vehicle host tests (no external deps). */

#include <math.h>
#include <stdio.h>
#include <string.h>

static int vt_checks;
static int vt_failures;

#define VT_CHECK(cond)                                                        \
    do {                                                                      \
        vt_checks++;                                                          \
        if (!(cond)) {                                                        \
            vt_failures++;                                                    \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,  \
                    #cond);                                                   \
        }                                                                     \
    } while (0)

#define VT_CHECK_MSG(cond, ...)                                               \
    do {                                                                      \
        vt_checks++;                                                          \
        if (!(cond)) {                                                        \
            vt_failures++;                                                    \
            fprintf(stderr, "%s:%d: check failed: %s: ", __FILE__, __LINE__,  \
                    #cond);                                                   \
            fprintf(stderr, __VA_ARGS__);                                     \
            fputc('\n', stderr);                                              \
        }                                                                     \
    } while (0)

#define VT_CHECK_INT(a, b)                                                    \
    do {                                                                      \
        long long vt_a_ = (long long)(a), vt_b_ = (long long)(b);             \
        VT_CHECK_MSG(vt_a_ == vt_b_, "%lld != %lld", vt_a_, vt_b_);          \
    } while (0)

#define VT_CHECK_FLT(a, b, tol)                                               \
    do {                                                                      \
        double vt_a_ = (double)(a), vt_b_ = (double)(b);                      \
        VT_CHECK_MSG(fabs(vt_a_ - vt_b_) <= (tol), "%.9g != %.9g", vt_a_,     \
                     vt_b_);                                                  \
    } while (0)

#define VT_RUN(fn)                                                            \
    do {                                                                      \
        int vt_before_ = vt_failures;                                         \
        fn();                                                                 \
        printf("%s %s\n", vt_failures == vt_before_ ? "PASS" : "FAIL", #fn); \
    } while (0)

#define VT_SUMMARY(name)                                                      \
    (printf("%s: %d checks, %d failures\n", name, vt_checks, vt_failures),   \
     vt_failures ? 1 : 0)
