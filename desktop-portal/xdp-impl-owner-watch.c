/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include "xdp-impl-owner-watch.h"

#include "xdp-types.h"

struct _XdpImplOwnerWatch
{
  GObject parent_instance;

  GDBusProxy *proxy;
  char *name_owner;
  guint subscription_id;
  GWeakRef object;
  XdpImplOwnerLostFunc owner_lost;
  gint lost;
  gint notified;
};

G_DEFINE_FINAL_TYPE (XdpImplOwnerWatch, xdp_impl_owner_watch, G_TYPE_OBJECT)

gboolean
xdp_impl_owner_watch_is_current (XdpImplOwnerWatch *watch)
{
  g_autofree char *name_owner = NULL;

  if (g_atomic_int_get (&watch->lost))
    return FALSE;

  name_owner = g_dbus_proxy_get_name_owner (watch->proxy);
  if (name_owner && g_strcmp0 (name_owner, watch->name_owner) == 0)
    return !g_atomic_int_get (&watch->lost);

  /* Once lost, even the original process reacquiring the name cannot revive
   * the requests and sessions belonging to its previous ownership. */
  g_atomic_int_set (&watch->lost, TRUE);
  return FALSE;
}

static void
on_name_owner_changed (GDBusConnection *connection,
                       const char      *sender_name,
                       const char      *object_path,
                       const char      *interface_name,
                       const char      *signal_name,
                       GVariant        *parameters,
                       gpointer         user_data)
{
  GWeakRef *weak_watch = user_data;
  g_autoptr(XdpImplOwnerWatch) watch = g_weak_ref_get (weak_watch);
  g_autoptr(GObject) object = NULL;
  const char *name, *old_owner, *new_owner;

  if (!watch)
    return;

  g_variant_get (parameters, "(&s&s&s)", &name, &old_owner, &new_owner);
  if (g_strcmp0 (new_owner, watch->name_owner) != 0)
    g_atomic_int_set (&watch->lost, TRUE);

  if (!g_atomic_int_get (&watch->lost) ||
      !g_atomic_int_compare_and_exchange (&watch->notified, FALSE, TRUE))
    return;

  object = g_weak_ref_get (&watch->object);
  if (object)
    watch->owner_lost (watch, object);
}

static void
weak_watch_free (gpointer data)
{
  GWeakRef *weak_watch = data;

  g_weak_ref_clear (weak_watch);
  g_free (weak_watch);
}

static void
xdp_impl_owner_watch_finalize (GObject *object)
{
  XdpImplOwnerWatch *watch = XDP_IMPL_OWNER_WATCH (object);

  g_dbus_connection_signal_unsubscribe (g_dbus_proxy_get_connection (watch->proxy),
                                         watch->subscription_id);
  g_weak_ref_clear (&watch->object);
  g_clear_object (&watch->proxy);
  g_free (watch->name_owner);

  G_OBJECT_CLASS (xdp_impl_owner_watch_parent_class)->finalize (object);
}

static void
xdp_impl_owner_watch_class_init (XdpImplOwnerWatchClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = xdp_impl_owner_watch_finalize;
}

static void
xdp_impl_owner_watch_init (XdpImplOwnerWatch *watch)
{
  g_weak_ref_init (&watch->object, NULL);
}

XdpImplOwnerWatch *
xdp_impl_owner_watch_new (GDBusProxy           *proxy,
                          GObject              *object,
                          XdpImplOwnerLostFunc   owner_lost)
{
  XdpImplOwnerWatch *watch;
  GWeakRef *weak_watch;

  watch = g_object_new (XDP_TYPE_IMPL_OWNER_WATCH, NULL);
  watch->proxy = g_object_ref (proxy);
  watch->name_owner = g_dbus_proxy_get_name_owner (proxy);
  watch->owner_lost = owner_lost;
  g_weak_ref_set (&watch->object, object);

  /* Subscribe directly: GDBusProxy may delay notify::g-name-owner until a
   * replacement backend answers GetAll. Weak references keep in-flight
   * callbacks safe if the watch or its target is finalized on another thread. */
  weak_watch = g_new0 (GWeakRef, 1);
  g_weak_ref_init (weak_watch, watch);
  watch->subscription_id =
    g_dbus_connection_signal_subscribe (g_dbus_proxy_get_connection (proxy),
                                         DBUS_DBUS_NAME,
                                         DBUS_DBUS_IFACE,
                                         "NameOwnerChanged",
                                         DBUS_DBUS_PATH,
                                         g_dbus_proxy_get_name (proxy),
                                         G_DBUS_SIGNAL_FLAGS_NONE,
                                         on_name_owner_changed,
                                         weak_watch, weak_watch_free);

  /* Also cover a change between capturing the owner and installing the watch,
   * or a proxy which already has no owner. Callers check before exporting. */
  xdp_impl_owner_watch_is_current (watch);

  return watch;
}

void
xdp_impl_owner_watch_close (XdpImplOwnerWatch *watch,
                            GDBusProxy        *impl_proxy)
{
  if (!xdp_impl_owner_watch_is_current (watch))
    return;

  g_dbus_connection_call (g_dbus_proxy_get_connection (impl_proxy),
                          watch->name_owner,
                          g_dbus_proxy_get_object_path (impl_proxy),
                          g_dbus_proxy_get_interface_name (impl_proxy),
                          "Close", NULL, G_VARIANT_TYPE_UNIT,
                          G_DBUS_CALL_FLAGS_NO_AUTO_START,
                          g_dbus_proxy_get_default_timeout (impl_proxy),
                          NULL, NULL, NULL);
}

gboolean
xdp_impl_owner_watch_close_sync (XdpImplOwnerWatch  *watch,
                                 GDBusProxy         *impl_proxy,
                                 GError            **error)
{
  g_autoptr(GVariant) reply = NULL;

  if (!xdp_impl_owner_watch_is_current (watch))
    return TRUE;

  reply = g_dbus_connection_call_sync (g_dbus_proxy_get_connection (impl_proxy),
                                       watch->name_owner,
                                       g_dbus_proxy_get_object_path (impl_proxy),
                                       g_dbus_proxy_get_interface_name (impl_proxy),
                                       "Close", NULL, G_VARIANT_TYPE_UNIT,
                                       G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                       g_dbus_proxy_get_default_timeout (impl_proxy),
                                       NULL, error);
  return reply != NULL;
}

DexFuture *
xdp_impl_owner_watch_close_future (XdpImplOwnerWatch *watch,
                                   GDBusProxy        *impl_proxy)
{
  if (!xdp_impl_owner_watch_is_current (watch))
    return dex_future_new_for_boolean (TRUE);

  return dex_dbus_connection_call (g_dbus_proxy_get_connection (impl_proxy),
                                   watch->name_owner,
                                   g_dbus_proxy_get_object_path (impl_proxy),
                                   g_dbus_proxy_get_interface_name (impl_proxy),
                                   "Close", NULL, G_VARIANT_TYPE_UNIT,
                                   G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                   g_dbus_proxy_get_default_timeout (impl_proxy));
}
