#include <config.h>

#include "location.h"

#include <libportal/portal.h>

extern char outdir[];

static int got_info = 0;

static void
location_cb (GObject *source,
             GAsyncResult *result,
             gpointer data)
{
  XdpPortal *portal = XDP_PORTAL (source);
  g_autoptr(GError) error = NULL;
  gboolean res;

  res = xdp_portal_location_monitor_start_finish (portal, result, &error);
  g_assert_true (res);
  g_assert_no_error (error);

  got_info = 1;
}

void
test_location_basic (void)
{
  g_autoptr(XdpPortal) portal = NULL;

#ifndef HAVE_GEOCLUE
  g_test_skip ("Skipping tests that require geoclue");
  return;
#endif

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_location_monitor_start (portal, NULL, 0, 0,  XDP_LOCATION_ACCURACY_EXACT, NULL, location_cb, NULL);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);

  xdp_portal_location_monitor_stop (portal);
}
