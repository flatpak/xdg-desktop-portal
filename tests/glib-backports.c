#include "glib-backports.h"

#if !GLIB_CHECK_VERSION(2,60,0)

gboolean
g_strv_equal (const gchar * const *strv1,
              const gchar * const *strv2)
{
  g_return_val_if_fail (strv1 != NULL, FALSE);
  g_return_val_if_fail (strv2 != NULL, FALSE);

  if (strv1 == strv2)
    return TRUE;

  for (; *strv1 != NULL && *strv2 != NULL; strv1++, strv2++)
    {
      if (!g_str_equal (*strv1, *strv2))
        return FALSE;
    }

  return (*strv1 == NULL && *strv2 == NULL);
}

#endif
