#ifndef BM_TEST_H
#define BM_TEST_H

#include <stdbool.h>
#include <stddef.h>

// The whole harness. A suite is a table of rows and one loop over it:
// every row is checked, nothing branches on a result, and the binary
// reports once at the end — so a broken path never hides the rows
// behind it. Failure lines carry table, row, want and got.
//
// Strings are printed escaped (`\xNN` for control bytes), because two
// of the surfaces under test speak in control bytes and a raw diff of
// those is unreadable.
void test_check_str(const char *table, const char *name,
    const char *want, const char *got);
void test_check_bool(const char *table, const char *name,
    bool want, bool got);
void test_check_sz(const char *table, const char *name,
    size_t want, size_t got);

// Abandon the suite because its fixture is unavailable — not because
// anything failed. Returns 77, which meson reads as SKIP.
int test_skip(const char *suite, const char *why);

// Exit status for the binary: 0 when every check passed, 1 otherwise.
int test_report(const char *suite);

#endif // BM_TEST_H
