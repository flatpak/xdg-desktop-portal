#pragma once

#include <glib.h>

#if !GLIB_CHECK_VERSION(2,60,0)
gboolean              g_strv_equal     (const gchar * const *strv1,
                                        const gchar * const *strv2);
#endif
