#include "config.h"

#include <glib.h>

#include "xdp-method-info.h"

static void
test_method_info_all (void)
{
  unsigned int i;
  unsigned int count = xdp_method_info_get_count ();
  const XdpMethodInfo *method_info = xdp_method_info_get_all ();

  g_assert_cmpint (count, >, 100);
  g_assert_nonnull (method_info);

  for (i = 0; i < count + 1; i++)
    {
      if (method_info->interface == NULL)
        return;
      g_assert_nonnull (method_info->method);
      method_info++;
    }

  g_assert_not_reached();
}

static void
test_method_info_find (void)
{
  const XdpMethodInfo *method_info;

  method_info = xdp_method_info_find ("org.freedesktop.portal.Notification", "AddNotification");
  g_assert_nonnull (method_info);
  g_assert_cmpint (method_info->option_arg, ==, -1);
  g_assert_false (method_info->uses_request);

  method_info = xdp_method_info_find ("org.freedesktop.portal.Inhibit", "Inhibit");
  g_assert_nonnull (method_info);
  g_assert_cmpint (method_info->option_arg, ==, 2);
  g_assert_true (method_info->uses_request);

  method_info = xdp_method_info_find ("org.freedesktop.portal.Inhibit", "QueryEndResponse");
  g_assert_nonnull (method_info);
  g_assert_cmpint (method_info->option_arg, ==, -1);
  g_assert_false (method_info->uses_request);

  /* Prefix is required */
  method_info = xdp_method_info_find ("Inhibit", "QueryEndResponse");
  g_assert_null (method_info);

  method_info = xdp_method_info_find ("DoesNotExist", "DoesNotExist");
  g_assert_null (method_info);

  method_info = xdp_method_info_find ("DoesNotExist", "DoesNotExist");
  g_assert_null (method_info);

  method_info = xdp_method_info_find ("org.freedesktop.portal.Inhibit", "DoesNotExist");
  g_assert_null (method_info);

}

int main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/method-info/all", test_method_info_all);
  g_test_add_func ("/method-info/find", test_method_info_find);
  return g_test_run ();
}
