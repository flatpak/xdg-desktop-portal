#include <gio/gio.h>

GDBusInterfaceSkeleton * screenshot_create (GDBusConnection *connection,
                                            const char      *dbus_name);
