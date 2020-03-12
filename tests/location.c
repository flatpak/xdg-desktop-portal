#include <config.h>

#include "location.h"

#include "src/xdp-utils.h"
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
  g_autoptr(GDBusConnection) system_bus = NULL;
  g_autoptr(GError) error = NULL;

#ifndef HAVE_GEOCLUE
  g_test_skip ("Skipping tests that require geoclue");
  return;
#endif

  system_bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);

  if (system_bus == NULL)
    {
      g_prefix_error (&error, "Unable to test Location without system bus: ");
      g_test_skip (error->message);
      return;
    }

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_location_monitor_start (portal, NULL, 0, 0,  XDP_LOCATION_ACCURACY_EXACT, 0, NULL, location_cb, NULL);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);

  xdp_portal_location_monitor_stop (portal);
}

static void
location_error (GObject *source,
                GAsyncResult *result,
                gpointer data)
{
  XdpPortal *portal = XDP_PORTAL (source);
  g_autoptr(GError) error = NULL;
  gboolean res;

  res = xdp_portal_location_monitor_start_finish (portal, result, &error);
  g_assert_false (res);
  g_assert_error (error, XDG_DESKTOP_PORTAL_ERROR, XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT);

  got_info = 1;
}

void
test_location_accuracy (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GDBusConnection) system_bus = NULL;
  g_autoptr(GError) error = NULL;

#ifndef HAVE_GEOCLUE
  g_test_skip ("Skipping tests that require geoclue");
  return;
#endif

  system_bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);

  if (system_bus == NULL)
    {
      g_prefix_error (&error, "Unable to test Location without system bus: ");
      g_test_skip (error->message);
      return;
    }

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_location_monitor_start (portal, NULL, 0, 0,  22, 0, NULL, location_error, NULL);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);

  xdp_portal_location_monitor_stop (portal);
}

