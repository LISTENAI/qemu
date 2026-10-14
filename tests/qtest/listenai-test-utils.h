/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TESTS_QTEST_LISTENAI_TEST_UTILS_H
#define TESTS_QTEST_LISTENAI_TEST_UTILS_H

#include <glib.h>
#include "libqtest.h"

/* Quote one argument for libqtest's platform-specific process launcher. */
static inline char *listenai_test_quote_arg(const char *arg)
{
#ifdef _WIN32
    GString *quoted = g_string_new("\"");
    gsize slashes = 0;

    for (const char *p = arg; ; p++) {
        if (*p == '\\') {
            slashes++;
            continue;
        }
        /* Escape quotes and double trailing backslashes before a quote. */
        gsize count = (*p == '"' || !*p) ? slashes * 2 : slashes;

        for (gsize i = 0; i < count; i++) {
            g_string_append_c(quoted, '\\');
        }
        slashes = 0;
        if (!*p) {
            break;
        }
        if (*p == '"') {
            g_string_append_c(quoted, '\\');
        }
        g_string_append_c(quoted, *p);
    }
    g_string_append_c(quoted, '"');
    return g_string_free(quoted, false);
#else
    return g_shell_quote(arg);
#endif
}

/* Let exit notifiers finish captures before releasing the test process. */
static inline void listenai_test_quit(QTestState *q)
{
    qtest_qmp_assert_success(q, "{'execute': 'quit'}");
    qtest_wait_qemu(q);
    qtest_quit(q);
}

#endif
