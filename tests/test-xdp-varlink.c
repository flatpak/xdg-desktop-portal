/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include <fcntl.h>
#include <sys/socket.h>

#include <glib.h>
#include <libdex.h>

#include "xdp-app-info-host-private.h"
#include "xdp-varlink-connection.h"
#include "xdp-varlink-instance.h"

typedef struct
{
  XdpAppInfoRegistry *app_info_registry;
  GHashTable *instances;
  int fds[2];
} Fixture;

static void
fixture_set_up (Fixture       *fixture,
                gconstpointer  unused)
{
  fixture->app_info_registry = xdp_app_info_registry_new ();
  fixture->instances = g_hash_table_new (g_str_hash, g_str_equal);
  g_assert_cmpint (socketpair (AF_UNIX, SOCK_STREAM, 0, fixture->fds), ==, 0);
}

static void
fixture_tear_down (Fixture       *fixture,
                   gconstpointer  unused)
{
  g_clear_pointer (&fixture->instances, g_hash_table_unref);
  g_clear_object (&fixture->app_info_registry);
  g_clear_fd (&fixture->fds[0], NULL);
  g_clear_fd (&fixture->fds[1], NULL);
}

static XdpVarlinkConnection *
take_connection (Fixture *fixture,
                 int      fd)
{
  XdpVarlinkConnection *connection;
  guint64 cookie;

  g_assert_true (xdp_varlink_socket_cookie (fd, &cookie));

  connection = xdp_varlink_connection_new (fixture->app_info_registry, fd, cookie);
  g_assert_nonnull (connection);

  return connection;
}

static XdpAppInfo *
host_app_info (const char *sender)
{
  int pidfd = -1;

  return xdp_app_info_host_new (sender, getpid (), &pidfd);
}

static void
test_cookie_distinct (Fixture       *fixture,
                      gconstpointer  unused)
{
  guint64 a, b;

  g_assert_true (xdp_varlink_socket_cookie (fixture->fds[0], &a));
  g_assert_true (xdp_varlink_socket_cookie (fixture->fds[1], &b));

  g_assert_cmpuint (a, !=, b);
  g_assert_cmpuint (a, !=, 0);
}

static void
test_cookie_not_a_socket (Fixture       *fixture,
                          gconstpointer  unused)
{
  g_autofd int fd = -1;
  guint64 cookie;

  fd = open ("/dev/null", O_RDONLY | O_CLOEXEC);
  g_assert_cmpint (fd, >=, 0);

  g_assert_false (xdp_varlink_socket_cookie (fd, &cookie));
  g_assert_false (xdp_varlink_socket_cookie (-1, &cookie));
}

static void
test_connection_peer_keys_differ (Fixture       *fixture,
                                  gconstpointer  unused)
{
  g_autoptr(XdpVarlinkConnection) a = take_connection (fixture, fixture->fds[0]);
  g_autoptr(XdpVarlinkConnection) b = take_connection (fixture, fixture->fds[1]);

  g_assert_cmpstr (xdp_peer_get_key (xdp_varlink_connection_get_peer (a)), !=,
                   xdp_peer_get_key (xdp_varlink_connection_get_peer (b)));
}

static void
on_closed (XdpVarlinkConnection *connection,
           unsigned int         *count)
{
  *count += 1;
}

static void
test_connection_closed_signal (Fixture       *fixture,
                               gconstpointer  unused)
{
  XdpVarlinkConnection *connection = take_connection (fixture, fixture->fds[0]);
  unsigned int closed = 0;

  g_signal_connect (connection, "closed", G_CALLBACK (on_closed), &closed);

  g_object_unref (connection);

  g_assert_cmpuint (closed, ==, 1);
}

static void
test_instance_lives_with_claimers (Fixture       *fixture,
                                   gconstpointer  unused)
{
  XdpVarlinkConnection *a = take_connection (fixture, fixture->fds[0]);
  XdpVarlinkConnection *b = take_connection (fixture, fixture->fds[1]);
  g_autoptr(XdpAppInfo) app_info = host_app_info ("test");
  XdpVarlinkInstance *instance;

  g_assert_null (xdp_varlink_connection_get_instance (a));
  g_assert_null (xdp_varlink_connection_get_app_info (a));

  instance = xdp_varlink_instance_new (fixture->instances, "1", app_info);
  xdp_varlink_connection_claim (a, instance);
  xdp_varlink_connection_claim (b, instance);
  g_object_unref (instance);

  g_assert_true (xdp_varlink_connection_get_app_info (a) == app_info);
  g_assert_nonnull (g_hash_table_lookup (fixture->instances, "1"));

  /* Survives while one claimer is left, and only then leaves the table */
  g_object_unref (a);
  g_assert_nonnull (g_hash_table_lookup (fixture->instances, "1"));

  g_object_unref (b);
  g_assert_null (g_hash_table_lookup (fixture->instances, "1"));
}

#define add_test(path, func) \
  g_test_add (path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  dex_init ();

  add_test ("/varlink/cookie/distinct", test_cookie_distinct);
  add_test ("/varlink/cookie/not-a-socket", test_cookie_not_a_socket);
  add_test ("/varlink/connection/peer-keys-differ",
            test_connection_peer_keys_differ);
  add_test ("/varlink/connection/closed-signal",
            test_connection_closed_signal);
  add_test ("/varlink/instance/lives-with-claimers",
            test_instance_lives_with_claimers);

  return g_test_run ();
}
