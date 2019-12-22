#pragma once

#include <gio/gio.h>

gboolean tests_set_property_sync (GDBusProxy *proxy,
                                  const char *iface,
                                  const char *property,
                                  GVariant *value,
                                  GError **error);
