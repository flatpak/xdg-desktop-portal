#define _GNU_SOURCE 1

#include "config.h"
#include <stdlib.h>

#include <gio/gio.h>

#include "xdp-impl-dbus.h"

#include "lockdown.h"

static void
property_changed (GObject *object,
                  GParamSpec *pspec,
                  gpointer data)
{
  gboolean value;

  g_object_get (object, pspec->name, &value, NULL);
  g_debug ("lockdown change: %s: %d", pspec->name, value);
}

void
lockdown_init (GDBusConnection *bus,
               const char *object_path)
{
  GDBusInterfaceSkeleton *helper;
  g_autoptr(GError) error = NULL;

  helper = G_DBUS_INTERFACE_SKELETON (xdp_dbus_impl_lockdown_skeleton_new ());

  if (!g_dbus_interface_skeleton_export (helper, bus, object_path, &error))
    {
      g_error ("Failed to export %s skeleton: %s\n",
               g_dbus_interface_skeleton_get_info (helper)->name,
               error->message);
      exit (1);
    }
  g_signal_connect (helper, "notify", G_CALLBACK (property_changed), NULL);

  g_debug ("providing %s at %s", g_dbus_interface_skeleton_get_info (helper)->name, object_path);
}

