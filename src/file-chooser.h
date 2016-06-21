#include <gio/gio.h>

GDBusInterfaceSkeleton * file_chooser_create (GDBusConnection *connection,
                                              const char      *dbus_name);
