#pragma once

#include <glib-object.h>

#include "xdp-diagnostic-dbus.h"

G_BEGIN_DECLS

#define XDP_TYPE_DIAGNOSTIC_DESKTOP (xdp_diagnostic_desktop_get_type())
G_DECLARE_FINAL_TYPE (XdpDiagnosticDesktop,
                      xdp_diagnostic_desktop,
                      XDP, DIAGNOSTIC_DESKTOP,
                      XdpDbusDiagnosticDesktopSkeleton)



XdpDiagnosticDesktop *xdp_diagnostic_desktop_get (GError **error);

void xdp_diagnostic_desktop_set_lockdown_impl (XdpDiagnosticDesktop *self,
                                               const char           *dbus_name);

void xdp_diagnostic_desktop_set_access_impl (XdpDiagnosticDesktop *self,
                                             const char           *dbus_name);

void xdp_diagnostic_desktop_set_portal_unique_impl (XdpDiagnosticDesktop *self,
                                                    const char           *portal_name,
                                                    const char           *impl_dbus_name,
                                                    unsigned int          version);

void xdp_diagnostic_desktop_add_portal_impl (XdpDiagnosticDesktop *self,
                                             const char           *portal_name,
                                             const char           *impl_dbus_name,
                                             unsigned int          version);

void xdp_diagnostic_desktop_update_properties (XdpDiagnosticDesktop *self);

G_END_DECLS
