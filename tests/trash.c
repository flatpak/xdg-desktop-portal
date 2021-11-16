#include <config.h>

#include "trash.h"

#include <libportal/portal.h>

static gboolean got_info;

static void
trash_cb (GObject *object,
          GAsyncResult *result,
          gpointer data)
{
  XdpPortal *portal = XDP_PORTAL (object);
  gboolean expected = *(gboolean*)data;
  gboolean ret;
  g_autoptr(GError) error = NULL;

  ret = xdp_portal_trash_file_finish (portal, result, &error);
  g_assert_cmpint (ret, ==, expected);
  if (ret)
    g_assert_no_error (error);
  else
    g_assert (error != NULL);

  got_info = TRUE;
  g_main_context_wakeup (NULL);
}

/* Reliably testing successful trashing in a CI environment
 * is hard, so just test something that is sure to fail.
 */
void
test_trash_file (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  gboolean expected;

  portal = xdp_portal_new ();

  expected = FALSE;
  got_info = FALSE;
  xdp_portal_trash_file (portal, "/proc/cmdline", NULL, trash_cb, &expected);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);    
}
