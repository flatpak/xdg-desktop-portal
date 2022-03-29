#include "config.h"
#include <stdlib.h>

#include <gio/gio.h>

#include "src/xdp-impl-dbus.h"
#include "tests/glib-backports.h"

#include "email.h"
#include "request.h"

typedef struct {
  XdpDbusImplEmail *impl;
  GDBusMethodInvocation *invocation;
  Request *request;
  GKeyFile *keyfile;
  char *app_id;
  GVariant *options;
  guint timeout;
} EmailHandle;

static void
email_handle_free (EmailHandle *handle)
{
  g_object_unref (handle->impl);
  g_object_unref (handle->request);
  g_key_file_unref (handle->keyfile);
  g_free (handle->app_id);
  g_variant_unref (handle->options);
  if (handle->timeout)
    g_source_remove (handle->timeout);

  g_free (handle);
}

static gboolean
send_response (gpointer data)
{
  EmailHandle *handle = data;
  GVariantBuilder opt_builder;
  const char *address = NULL;
  const char *subject = NULL;
  const char *body = NULL;
  const char *no_att[1] = { NULL };
  const char **attachments = no_att;
  char *s;
  int response;
  const char * const *addresses;
  const char * const *cc;
  const char * const *bcc;
  char **strv;

  g_variant_lookup (handle->options, "address", "&s", &address);
  g_variant_lookup (handle->options, "subject", "&s", &subject);
  g_variant_lookup (handle->options, "body", "&s", &body);
  g_variant_lookup (handle->options, "attachments", "^a&s", &attachments);
  g_variant_lookup (handle->options, "addresses", "^a&s", &addresses);
  g_variant_lookup (handle->options, "cc", "^a&s", &cc);
  g_variant_lookup (handle->options, "bcc", "^a&s", &bcc);

  if (g_key_file_get_boolean (handle->keyfile, "backend", "expect-close", NULL))
    g_assert_not_reached ();

  s = g_key_file_get_string (handle->keyfile, "input", "subject", NULL);
  g_assert_cmpstr (s, ==, subject);
  g_free (s);
  s = g_key_file_get_string (handle->keyfile, "input", "body", NULL);
  g_assert_cmpstr (s, ==, body);
  g_free (s);

  strv = g_key_file_get_string_list (handle->keyfile, "input", "addresses", NULL, NULL);
  if (strv)
    {
      g_assert (addresses != NULL);
      g_assert_true (g_strv_equal ((const char * const *)strv, addresses));
      g_strfreev (strv);
    }

  strv = g_key_file_get_string_list (handle->keyfile, "input", "cc", NULL, NULL);
  if (strv)
    {
      g_assert (addresses != NULL);
      g_assert_true (g_strv_equal ((const char * const *)strv, cc));
      g_strfreev (strv);
    }

  strv = g_key_file_get_string_list (handle->keyfile, "input", "bcc", NULL, NULL);
  if (strv)
    {
      g_assert (addresses != NULL);
      g_assert_true (g_strv_equal ((const char * const *)strv, bcc));
      g_strfreev (strv);
    }

  /* fixme: attachments */

  response = g_key_file_get_integer (handle->keyfile, "backend", "response", NULL);

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

  if (handle->request->exported)
    request_unexport (handle->request);

  g_debug ("send response %d", response);

  xdp_dbus_impl_email_complete_compose_email (handle->impl,
                                              handle->invocation,
                                              response,
                                              g_variant_builder_end (&opt_builder));

  handle->timeout = 0;

  email_handle_free (handle);

  return G_SOURCE_REMOVE;
}

static gboolean
handle_close (XdpDbusImplRequest *object,
              GDBusMethodInvocation *invocation,
              EmailHandle *handle)
{
  GVariantBuilder opt_builder;

  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);
  g_debug ("send response 2");
  xdp_dbus_impl_email_complete_compose_email (handle->impl,
                                              handle->invocation,
                                              2,
                                              g_variant_builder_end (&opt_builder));
  email_handle_free (handle);

  return FALSE;
}

static gboolean
handle_compose_email (XdpDbusImplEmail *object,
                      GDBusMethodInvocation *invocation,
                      const char *arg_handle,
                      const char *arg_app_id,
                      const char *arg_parent_window,
                      GVariant *arg_options)
{
  g_autoptr(Request) request = NULL;
  const char *sender;
  g_autoptr(GError) error = NULL;
  const char *dir;
  g_autofree char *path = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  EmailHandle *handle;
  int delay;

  g_debug ("Handling ComposeEmail");

  sender = g_dbus_method_invocation_get_sender (invocation);

  dir = g_getenv ("XDG_DATA_HOME");
  path = g_build_filename (dir, "email", NULL);
  keyfile = g_key_file_new ();
  g_key_file_load_from_file (keyfile, path, 0, &error);
  g_assert_no_error (error);

  request = request_new (sender, arg_app_id, arg_handle);

  handle = g_new (EmailHandle, 1);
  handle->impl = g_object_ref (object);
  handle->invocation = invocation;
  handle->request = g_object_ref (request);
  handle->keyfile = g_key_file_ref (keyfile);
  handle->app_id = g_strdup (arg_app_id);
  handle->options = g_variant_ref (arg_options);

  g_signal_connect (request, "handle-close", G_CALLBACK (handle_close), handle);

  request_export (request, g_dbus_method_invocation_get_connection (invocation));

  if (g_key_file_has_key (keyfile, "backend", "delay", NULL))
    delay = g_key_file_get_integer (keyfile, "backend", "delay", NULL);
  else
    delay = 200;

  g_debug ("delay %d", delay);

  if (delay == 0)
    send_response (handle);
  else
    handle->timeout = g_timeout_add (delay, send_response, handle);

  return TRUE;
}

void
email_init (GDBusConnection *bus,
            const char *object_path)
{
  g_autoptr(GError) error = NULL;
  GDBusInterfaceSkeleton *helper;

  helper = G_DBUS_INTERFACE_SKELETON (xdp_dbus_impl_email_skeleton_new ());

  g_signal_connect (helper, "handle-compose-email", G_CALLBACK (handle_compose_email), NULL);

  if (!g_dbus_interface_skeleton_export (helper, bus, object_path, &error))
    {
      g_error ("Failed to export %s skeleton: %s\n",
               g_dbus_interface_skeleton_get_info (helper)->name,
               error->message);
      exit (1);
    }

  g_debug ("providing %s at %s", g_dbus_interface_skeleton_get_info (helper)->name, object_path);
}
