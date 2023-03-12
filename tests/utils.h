#pragma once

#include <gio/gio.h>

gboolean tests_set_property_sync (GDBusProxy *proxy,
                                  const char *iface,
                                  const char *property,
                                  GVariant *value,
                                  GError **error);

void setup_dbus_daemon_wrapper (const char *outdir);

gboolean tests_set_app_id (const char *app_id, GError **error);
gboolean tests_set_app_desktop_id (const char *desktop_id, GError **error);

const char *tests_get_expected_app_id (void);
const char *tests_get_expected_desktop_id (void);
