/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <glib.h>
#include <string.h>

typedef enum UnescapeFlags {
        UNESCAPE_RELAX      = 1 << 0,
        UNESCAPE_ACCEPT_NUL = 1 << 1,
} UnescapeFlags;

gssize cunescape_length_with_prefix(const char *s, gsize length, const char *prefix, UnescapeFlags flags, char **ret);
static inline gssize cunescape_length(const char *s, gsize length, UnescapeFlags flags, char **ret) {
        return cunescape_length_with_prefix(s, length, NULL, flags, ret);
}
static inline gssize cunescape(const char *s, UnescapeFlags flags, char **ret) {
        return cunescape_length(s, strlen(s), flags, ret);
}
