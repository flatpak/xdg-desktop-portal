#include <config.h>

#include "emulated-input.h"

#include <libportal/portal.h>
#include "src/xdp-utils.h"
#include "src/xdp-impl-dbus.h"

#include "utils.h"

extern XdpImplPermissionStore *permission_store;
extern XdpImplLockdown *lockdown;

static int got_info;

extern char outdir[];

static void
emulated_input_cb (GObject      *obj,
                   GAsyncResult *result,
                   gpointer      data)
{
  XdpPortal *portal = XDP_PORTAL (obj);
  g_autoptr(GError) error = NULL;
  GKeyFile *keyfile = data;
  int response;
  int domain;
  int code;
  gboolean ret;

  response = g_key_file_get_integer (keyfile, "result", "response", NULL);
  domain = g_key_file_get_integer (keyfile, "result", "error_domain", NULL);
  code = g_key_file_get_integer (keyfile, "result", "error_code", NULL);

  ret = xdp_portal_emulated_input_finish (portal, result, &error);

  g_debug ("emulated_input cb: %d", g_key_file_get_integer (keyfile, "result", "marker", NULL));
  if (response == 0)
    {
      g_assert_true (ret);
      g_assert_no_error (error);
    }
  else if (response == 1)
    {
      g_assert_false (ret);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    }
  else if (response == 2)
    {
      g_assert_false (ret);
      g_assert_error (error, domain, code);
    }
  else
    g_assert_not_reached ();

  got_info++;

  g_main_context_wakeup (NULL);
}

void
test_emulated_input_basic (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;

  keyfile = g_key_file_new ();
  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "emulated-input", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  got_info = 0;
  xdp_portal_emulated_input (portal, 0, NULL, emulated_input_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}
