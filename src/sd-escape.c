/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "sd-escape.h"
#include <errno.h>

/* The following is copied from these files in systemd with minor
 * modifications:
 * - src/basic/escape.c
 * - src/basic/utf8.c
 * - src/basic/hexdecoct.c
 */

static int unhexchar(char c) {

        if (c >= '0' && c <= '9')
                return c - '0';

        if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;

        if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;

        return -EINVAL;
}

static gboolean
unichar_is_valid(guint32 ch) {

        if (ch >= 0x110000) /* End of unicode space */
                return FALSE;
        if ((ch & 0xFFFFF800) == 0xD800) /* Reserved area for UTF-16 */
                return FALSE;
        if ((ch >= 0xFDD0) && (ch <= 0xFDEF)) /* Reserved */
                return FALSE;
        if ((ch & 0xFFFE) == 0xFFFE) /* BOM (Byte Order Mark) */
                return FALSE;

        return TRUE;
}

static int
unoctchar(char c) {

        if (c >= '0' && c <= '7')
                return c - '0';

        return -EINVAL;
}

int cunescape_one(const char *p, gsize length, guint32 *ret, gboolean *eight_bit, gboolean accept_nul) {
        int r = 1;

        g_assert(p);
        g_assert(ret);

        /* Unescapes C style. Returns the unescaped character in ret.
         * Sets *eight_bit to true if the escaped sequence either fits in
         * one byte in UTF-8 or is a non-unicode literal byte and should
         * instead be copied directly.
         */

        if (length != G_MAXSIZE && length < 1)
                return -EINVAL;

        switch (p[0]) {

        case 'a':
                *ret = '\a';
                break;
        case 'b':
                *ret = '\b';
                break;
        case 'f':
                *ret = '\f';
                break;
        case 'n':
                *ret = '\n';
                break;
        case 'r':
                *ret = '\r';
                break;
        case 't':
                *ret = '\t';
                break;
        case 'v':
                *ret = '\v';
                break;
        case '\\':
                *ret = '\\';
                break;
        case '"':
                *ret = '"';
                break;
        case '\'':
                *ret = '\'';
                break;

        case 's':
                /* This is an extension of the XDG syntax files */
                *ret = ' ';
                break;

        case 'x': {
                /* hexadecimal encoding */
                int a, b;

                if (length != G_MAXSIZE && length < 3)
                        return -EINVAL;

                a = unhexchar(p[1]);
                if (a < 0)
                        return -EINVAL;

                b = unhexchar(p[2]);
                if (b < 0)
                        return -EINVAL;

                /* Don't allow NUL bytes */
                if (a == 0 && b == 0 && !accept_nul)
                        return -EINVAL;

                *ret = (a << 4U) | b;
                *eight_bit = TRUE;
                r = 3;
                break;
        }

        case 'u': {
                /* C++11 style 16bit unicode */

                int a[4];
                gsize i;
                guint32 c;

                if (length != G_MAXSIZE && length < 5)
                        return -EINVAL;

                for (i = 0; i < 4; i++) {
                        a[i] = unhexchar(p[1 + i]);
                        if (a[i] < 0)
                                return a[i];
                }

                c = ((guint32) a[0] << 12U) | ((guint32) a[1] << 8U) | ((guint32) a[2] << 4U) | (guint32) a[3];

                /* Don't allow 0 chars */
                if (c == 0 && !accept_nul)
                        return -EINVAL;

                *ret = c;
                r = 5;
                break;
        }

        case 'U': {
                /* C++11 style 32bit unicode */

                int a[8];
                gsize i;
                guint32 c;

                if (length != G_MAXSIZE && length < 9)
                        return -EINVAL;

                for (i = 0; i < 8; i++) {
                        a[i] = unhexchar(p[1 + i]);
                        if (a[i] < 0)
                                return a[i];
                }

                c = ((guint32) a[0] << 28U) | ((guint32) a[1] << 24U) | ((guint32) a[2] << 20U) | ((guint32) a[3] << 16U) |
                    ((guint32) a[4] << 12U) | ((guint32) a[5] <<  8U) | ((guint32) a[6] <<  4U) |  (guint32) a[7];

                /* Don't allow 0 chars */
                if (c == 0 && !accept_nul)
                        return -EINVAL;

                /* Don't allow invalid code points */
                if (!unichar_is_valid(c))
                        return -EINVAL;

                *ret = c;
                r = 9;
                break;
        }

        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7': {
                /* octal encoding */
                int a, b, c;
                guint32 m;

                if (length != G_MAXSIZE && length < 3)
                        return -EINVAL;

                a = unoctchar(p[0]);
                if (a < 0)
                        return -EINVAL;

                b = unoctchar(p[1]);
                if (b < 0)
                        return -EINVAL;

                c = unoctchar(p[2]);
                if (c < 0)
                        return -EINVAL;

                /* don't allow NUL bytes */
                if (a == 0 && b == 0 && c == 0 && !accept_nul)
                        return -EINVAL;

                /* Don't allow bytes above 255 */
                m = ((guint32) a << 6U) | ((guint32) b << 3U) | (guint32) c;
                if (m > 255)
                        return -EINVAL;

                *ret = m;
                *eight_bit = TRUE;
                r = 3;
                break;
        }

        default:
                return -EINVAL;
        }

        return r;
}

/**
 * utf8_encode_unichar() - Encode single UCS-4 character as UTF-8
 * @out_utf8: output buffer of at least 4 bytes or NULL
 * @g: UCS-4 character to encode
 *
 * This encodes a single UCS-4 character as UTF-8 and writes it into @out_utf8.
 * The length of the character is returned. It is not zero-terminated! If the
 * output buffer is NULL, only the length is returned.
 *
 * Returns: The length in bytes that the UTF-8 representation does or would
 *          occupy.
 */
static gsize
utf8_encode_unichar(char *out_utf8, guint32 g) {

        if (g < (1 << 7)) {
                if (out_utf8)
                        out_utf8[0] = g & 0x7f;
                return 1;
        } else if (g < (1 << 11)) {
                if (out_utf8) {
                        out_utf8[0] = 0xc0 | ((g >> 6) & 0x1f);
                        out_utf8[1] = 0x80 | (g & 0x3f);
                }
                return 2;
        } else if (g < (1 << 16)) {
                if (out_utf8) {
                        out_utf8[0] = 0xe0 | ((g >> 12) & 0x0f);
                        out_utf8[1] = 0x80 | ((g >> 6) & 0x3f);
                        out_utf8[2] = 0x80 | (g & 0x3f);
                }
                return 3;
        } else if (g < (1 << 21)) {
                if (out_utf8) {
                        out_utf8[0] = 0xf0 | ((g >> 18) & 0x07);
                        out_utf8[1] = 0x80 | ((g >> 12) & 0x3f);
                        out_utf8[2] = 0x80 | ((g >> 6) & 0x3f);
                        out_utf8[3] = 0x80 | (g & 0x3f);
                }
                return 4;
        }

        return 0;
}

gssize cunescape_length_with_prefix(const char *s, gsize length, const char *prefix, UnescapeFlags flags, char **ret) {
        g_autofree char *ans = NULL;
        char *t;
        const char *f;
        gsize pl;
        int r;

        g_assert(s);
        g_assert(ret);

        /* Undoes C style string escaping, and optionally prefixes it. */

        if (!prefix)
          pl = 0;
        else
          pl = strlen(prefix);

        ans = g_new(char, pl+length+1);
        if (!ans)
                return -ENOMEM;

        if (prefix)
                memcpy(ans, prefix, pl);

        for (f = s, t = ans + pl; f < s + length; f++) {
                gsize remaining;
                gboolean eight_bit = FALSE;
                guint32 u;

                remaining = s + length - f;
                g_assert(remaining > 0);

                if (*f != '\\') {
                        /* A literal, copy verbatim */
                        *(t++) = *f;
                        continue;
                }

                if (remaining == 1) {
                        if (flags & UNESCAPE_RELAX) {
                                /* A trailing backslash, copy verbatim */
                                *(t++) = *f;
                                continue;
                        }

                        return -EINVAL;
                }

                r = cunescape_one(f + 1, remaining - 1, &u, &eight_bit, flags & UNESCAPE_ACCEPT_NUL);
                if (r < 0) {
                        if (flags & UNESCAPE_RELAX) {
                                /* Invalid escape code, let's take it literal then */
                                *(t++) = '\\';
                                continue;
                        }

                        return r;
                }

                f += r;
                if (eight_bit)
                        /* One byte? Set directly as specified */
                        *(t++) = u;
                else
                        /* Otherwise encode as multi-byte UTF-8 */
                        t += utf8_encode_unichar(t, u);
        }

        *t = 0;

        g_assert(t >= ans); /* Let static analyzers know that the answer is non-negative. */
        *ret = g_steal_pointer (&ans);
        return t - *ret;
}
