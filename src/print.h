#include <gio/gio.h>

GDBusInterfaceSkeleton * print_create (GDBusConnection *connection,
                                       const char      *dbus_name);
