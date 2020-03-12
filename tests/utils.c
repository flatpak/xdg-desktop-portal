#include <config.h>

#include "utils.h"

/*
 * Set a property. Unlike gdbus-codegen-generated wrapper functions, this
 * waits for the property change to take effect.
 *
 * If @value is floating, ownership is taken.
 */
gboolean
tests_set_property_sync (GDBusProxy *proxy,
                         const char *iface,
                         const char *property,
                         GVariant *value,
                         GError **error)
{
  g_autoptr (GVariant) res = NULL;

  res = g_dbus_proxy_call_sync (proxy,
                                "org.freedesktop.DBus.Properties.Set",
                                g_variant_new ("(ssv)", iface, property, value),
                                G_DBUS_CALL_FLAGS_NONE,
                                -1,
                                NULL,
                                error);
  return (res != NULL);
}
