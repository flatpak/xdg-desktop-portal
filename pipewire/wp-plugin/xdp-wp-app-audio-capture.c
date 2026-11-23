/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors */

#include "xdp-wp-app-audio-capture.h"

#include "xdp-utils.h"

WP_DEFINE_LOCAL_LOG_TOPIC ("m-xdp-app-audio-capture");

struct _XdpWpAppAudioCapture
{
  XdpDbusImplAppAudioCaptureSkeleton parent_instance;

  WpCore *core;
  GDBusConnection *connection;
  guint name_id;
};

static void xdp_wp_app_audio_capture_iface_init (XdpDbusImplAppAudioCaptureIface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (XdpWpAppAudioCapture,
                               xdp_wp_app_audio_capture,
                               XDP_DBUS_IMPL_TYPE_APP_AUDIO_CAPTURE_SKELETON,
                               G_IMPLEMENT_INTERFACE (XDP_DBUS_IMPL_TYPE_APP_AUDIO_CAPTURE,
                                                      xdp_wp_app_audio_capture_iface_init))

typedef enum
{
  PROP_CORE = 1,
  PROP_CONNECTION,
} XdpWpAppAudioCaptureProps;

static GParamSpec *props[PROP_CONNECTION + 1] = { NULL, };

static gboolean
handle_create_session (XdpDbusImplAppAudioCapture *object,
                       GDBusMethodInvocation      *invocation,
                       const char                 *arg_handle,
                       const char                 *arg_app_id,
                       GVariant                   *arg_options)
{
  g_dbus_method_invocation_return_error_literal (invocation, XDG_DESKTOP_PORTAL_ERROR,
                                                 XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                                 "Not implemented yet");
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_create_system_stream (XdpDbusImplAppAudioCapture *object,
                             GDBusMethodInvocation      *invocation,
                             const char                 *arg_session_handle,
                             GVariant                   *arg_options)
{
  g_dbus_method_invocation_return_error_literal (invocation, XDG_DESKTOP_PORTAL_ERROR,
                                                 XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                                 "Not implemented yet");
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
handle_create_application_stream (XdpDbusImplAppAudioCapture *object,
                                  GDBusMethodInvocation      *invocation,
                                  const char                 *arg_session_handle,
                                  GVariant                   *arg_options)
{
  g_dbus_method_invocation_return_error_literal (invocation, XDG_DESKTOP_PORTAL_ERROR,
                                                 XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                                 "Not implemented yet");
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
xdp_wp_app_audio_capture_iface_init (XdpDbusImplAppAudioCaptureIface *iface)
{
  iface->handle_create_session = handle_create_session;
  iface->handle_create_system_stream = handle_create_system_stream;
  iface->handle_create_application_stream = handle_create_application_stream;
}

static void
on_name_lost (GDBusConnection *connection,
              const char      *name,
              gpointer         user_data)
{
  wp_warning_object (user_data, "Lost the name %s", name);
}

static void
xdp_wp_app_audio_capture_set_property (GObject      *object,
                                       guint         property_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  XdpWpAppAudioCapture *self = XDP_WP_APP_AUDIO_CAPTURE (object);

  switch ((XdpWpAppAudioCaptureProps) property_id)
    {
    case PROP_CORE:
      self->core = g_value_dup_object (value);
      break;

    case PROP_CONNECTION:
      self->connection = g_value_dup_object (value);
      break;
    }
}

static void
xdp_wp_app_audio_capture_constructed (GObject *object)
{
  XdpWpAppAudioCapture *self = XDP_WP_APP_AUDIO_CAPTURE (object);
  g_autoptr (GError) error = NULL;

  G_OBJECT_CLASS (xdp_wp_app_audio_capture_parent_class)->constructed (object);

  g_assert (WP_IS_CORE (self->core));
  g_assert (G_IS_DBUS_CONNECTION (self->connection));

  xdp_dbus_impl_app_audio_capture_set_version (XDP_DBUS_IMPL_APP_AUDIO_CAPTURE (self), 1);
  if (!g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (self), self->connection,
                                         DESKTOP_DBUS_PATH, &error))
    {
      wp_warning_object (self, "Can't export %s: %s",
                         DESKTOP_DBUS_PATH, error->message);
      return;
    }
  self->name_id = g_bus_own_name_on_connection (self->connection,
                                                APP_AUDIO_CAPTURE_DBUS_IMPL_IFACE,
                                                G_BUS_NAME_OWNER_FLAGS_NONE,
                                                NULL, on_name_lost, self, NULL);
}

static void
xdp_wp_app_audio_capture_dispose (GObject *object)
{
  XdpWpAppAudioCapture *self = XDP_WP_APP_AUDIO_CAPTURE (object);

  g_clear_handle_id (&self->name_id, g_bus_unown_name);
  if (g_dbus_interface_skeleton_get_connection (G_DBUS_INTERFACE_SKELETON (self)))
    g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (self));

  G_OBJECT_CLASS (xdp_wp_app_audio_capture_parent_class)->dispose (object);
}

static void
xdp_wp_app_audio_capture_finalize (GObject *object)
{
  XdpWpAppAudioCapture *self = XDP_WP_APP_AUDIO_CAPTURE (object);

  g_clear_object (&self->connection);
  g_clear_object (&self->core);

  G_OBJECT_CLASS (xdp_wp_app_audio_capture_parent_class)->finalize (object);
}

static void
xdp_wp_app_audio_capture_class_init (XdpWpAppAudioCaptureClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->set_property = xdp_wp_app_audio_capture_set_property;
  object_class->constructed = xdp_wp_app_audio_capture_constructed;
  object_class->dispose = xdp_wp_app_audio_capture_dispose;
  object_class->finalize = xdp_wp_app_audio_capture_finalize;

  props[PROP_CORE] = g_param_spec_object ("core", NULL, NULL,
                                          WP_TYPE_CORE,
                                          G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
  props[PROP_CONNECTION] = g_param_spec_object ("connection", NULL, NULL,
                                                G_TYPE_DBUS_CONNECTION,
                                                G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, G_N_ELEMENTS (props), props);
}

static void
xdp_wp_app_audio_capture_init (XdpWpAppAudioCapture *self)
{
}

XdpWpAppAudioCapture *
xdp_wp_app_audio_capture_new (WpCore          *core,
                              GDBusConnection *connection)
{
  return g_object_new (XDP_WP_TYPE_APP_AUDIO_CAPTURE,
                       "core", core,
                       "connection", connection,
                       NULL);
}
