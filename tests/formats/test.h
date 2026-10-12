/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* Minimal checking for the formats and export tests: CHECK records a
 * failure and carries on, so one run reports everything that is wrong;
 * the test returns the failure count (0 passes). Tests run from the source
 * root (meson workdir) so tests/data is a relative path. */

#ifndef RXBOX_TESTS_FORMATS_TEST_H
#define RXBOX_TESTS_FORMATS_TEST_H

#include <stdio.h>
#include <string.h>

#include "rxbox/common.h"

static int failures;

#define CHECK(cond) do {                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                       \
        }                                                                     \
    } while (0)

#define CHECK_EQ(a, b) do {                                                   \
        long long va_ = (long long)(a), vb_ = (long long)(b);                 \
        if (va_ != vb_) {                                                     \
            fprintf(stderr, "%s:%d: %s == %s failed: %lld != %lld\n",         \
                    __FILE__, __LINE__, #a, #b, va_, vb_);                    \
            failures++;                                                       \
        }                                                                     \
    } while (0)

#define CHECK_STR(a, b) do {                                                  \
        const char *sa_ = (a), *sb_ = (b);                                    \
        if (!sa_ || !sb_ || strcmp(sa_, sb_)) {                               \
            fprintf(stderr, "%s:%d: %s == %s failed: \"%s\" != \"%s\"\n",     \
                    __FILE__, __LINE__, #a, #b, sa_ ? sa_ : "(null)", sb_ ? sb_ : "(null)"); \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static inline int test_result(const char *name)
{
    if (failures)
        fprintf(stderr, "%s: %d failure(s)\n", name, failures);
    else
        fprintf(stderr, "%s: ok\n", name);
    return failures ? 1 : 0;
}

#endif /* RXBOX_TESTS_FORMATS_TEST_H */
