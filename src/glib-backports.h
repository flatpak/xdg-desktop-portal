/*
 * Copyright © 2014, 2016 Red Hat, Inc
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *       Alexander Larsson <alexl@redhat.com>
 *       Matthias Clasen <mclasen@redhat.com>
 */

#pragma once

#include <gio/gio.h>

#if !GLIB_CHECK_VERSION (2, 68, 0)
guint g_string_replace (GString     *string,
                        const gchar *find,
                        const gchar *replace,
                        guint        limit);
#endif

#if !GLIB_CHECK_VERSION (2, 68, 0)
static inline void
g_log_writer_default_set_use_stderr (gboolean use_stderr)
{
  /* Does nothing because outside of the tests we don't really care that it
   * doesn't work correctly after this call and those tests can run on newer
   * GLibs
   */
}
#endif
