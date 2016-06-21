#include <gio/gio.h>

GDBusInterfaceSkeleton * open_uri_create (GDBusConnection *connection,
                                          const char      *dbus_name);

