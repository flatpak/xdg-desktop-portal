#include <config.h>

#include "account.h"

#include <libportal/portal.h>
#include "xdp-impl-dbus.h"

extern char outdir[];

static int got_info = 0;

extern XdpDbusImplPermissionStore *permission_store;
extern gchar *appid;

static void
set_wallpaper_permissions (const char *permission)
{
  const char *permissions[2] = { NULL, NULL };
  g_autoptr(GError) error = NULL;

  permissions[0] = permission;
  xdp_dbus_impl_permission_store_call_set_permission_sync (permission_store,
                                                           "wallpaper",
                                                           TRUE,
                                                           "wallpaper",
                                                           appid,
                                                           permissions,
                                                           NULL,
                                                           &error);
  g_assert_no_error (error);
}

static void
reset_wallpaper_permissions (void)
{
  set_wallpaper_permissions (NULL);
}

static void
wallpaper_cb (GObject *obj,
              GAsyncResult *result,
              gpointer data)
{
  XdpPortal *portal = XDP_PORTAL (obj);
  g_autoptr(GError) error = NULL;
  GKeyFile *keyfile = data;
  gboolean res;
  int response;

  response = g_key_file_get_integer (keyfile, "result", "response", NULL);

  res = xdp_portal_set_wallpaper_finish (portal, result, &error);
  if (response == 0)
    {
      g_assert_true (res);
      g_assert_no_error (error);
    }
  else if (response == 1)
    {
      g_assert_false (res);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    }
  else if (response == 2)
    {
      g_assert_false (res);
      g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
    }
  else
    g_assert_not_reached ();

  got_info++;

  g_main_context_wakeup (NULL);
}

static const char *
target_to_string (XdpWallpaperFlags target)
{
  const char *strings[] = { "", "background", "lockscreen", "both" };
  return strings[target & 3];
}

void
test_wallpaper_basic (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  g_autofree char *uri = NULL;
  XdpWallpaperFlags target = XDP_WALLPAPER_FLAG_BACKGROUND | XDP_WALLPAPER_FLAG_LOCKSCREEN;

  reset_wallpaper_permissions ();

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "access", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);
  g_free (path);

  g_key_file_set_string (keyfile, "wallpaper", "target", target_to_string (target));
  g_key_file_set_boolean (keyfile, "wallpaper", "preview", FALSE);

  path = g_build_filename (outdir, "wallpaper", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  uri = g_strconcat ("file://", path, NULL);

  got_info = 0;
  xdp_portal_set_wallpaper (portal, NULL, uri, target, NULL, wallpaper_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

void
test_wallpaper_delay (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  g_autofree char *uri = NULL;
  XdpWallpaperFlags target = XDP_WALLPAPER_FLAG_LOCKSCREEN;

  reset_wallpaper_permissions ();

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "access", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);
  g_free (path);

  g_key_file_set_string (keyfile, "wallpaper", "target", target_to_string (target));
  g_key_file_set_boolean (keyfile, "wallpaper", "preview", FALSE);

  path = g_build_filename (outdir, "wallpaper", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  uri = g_strconcat ("file://", path, NULL);

  got_info = 0;
  xdp_portal_set_wallpaper (portal, NULL, uri, target, NULL, wallpaper_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

void
test_wallpaper_cancel1 (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  g_autofree char *uri = NULL;
  XdpWallpaperFlags target = XDP_WALLPAPER_FLAG_BACKGROUND;

  reset_wallpaper_permissions ();

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 1);
  g_key_file_set_integer (keyfile, "result", "response", 1);

  path = g_build_filename (outdir, "access", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);
  g_free (path);

  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 2);
  g_key_file_set_string (keyfile, "wallpaper", "target", target_to_string (target));
  g_key_file_set_boolean (keyfile, "wallpaper", "preview", FALSE);

  path = g_build_filename (outdir, "wallpaper", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  uri = g_strconcat ("file://", path, NULL);

  got_info = 0;
  xdp_portal_set_wallpaper (portal, NULL, uri, target, NULL, wallpaper_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

void
test_wallpaper_cancel2 (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  g_autofree char *uri = NULL;
  XdpWallpaperFlags target = XDP_WALLPAPER_FLAG_BACKGROUND | XDP_WALLPAPER_FLAG_LOCKSCREEN | XDP_WALLPAPER_FLAG_PREVIEW;

  reset_wallpaper_permissions ();

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "access", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);
  g_free (path);

  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 1);
  g_key_file_set_integer (keyfile, "result", "response", 1);
  g_key_file_set_string (keyfile, "wallpaper", "target", target_to_string (target));
  g_key_file_set_boolean (keyfile, "wallpaper", "preview", TRUE);

  path = g_build_filename (outdir, "wallpaper", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  uri = g_strconcat ("file://", path, NULL);

  got_info = 0;
  xdp_portal_set_wallpaper (portal, NULL, uri, target, NULL, wallpaper_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}

void
test_wallpaper_permission (void)
{
  g_autoptr(XdpPortal) portal = NULL;
  g_autoptr(GKeyFile) keyfile = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree char *path = NULL;
  g_autofree char *uri = NULL;
  XdpWallpaperFlags target = XDP_WALLPAPER_FLAG_BACKGROUND | XDP_WALLPAPER_FLAG_LOCKSCREEN | XDP_WALLPAPER_FLAG_PREVIEW;

  set_wallpaper_permissions ("no");

  keyfile = g_key_file_new ();

  g_key_file_set_integer (keyfile, "backend", "delay", 0);
  g_key_file_set_integer (keyfile, "backend", "response", 0);
  g_key_file_set_integer (keyfile, "result", "response", 0);

  path = g_build_filename (outdir, "access", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);
  g_free (path);

  g_key_file_set_integer (keyfile, "backend", "delay", 200);
  g_key_file_set_integer (keyfile, "backend", "response", 1);
  g_key_file_set_integer (keyfile, "result", "response", 2);
  g_key_file_set_string (keyfile, "wallpaper", "target", target_to_string (target));
  g_key_file_set_boolean (keyfile, "wallpaper", "preview", TRUE);

  path = g_build_filename (outdir, "wallpaper", NULL);
  g_key_file_save_to_file (keyfile, path, &error);
  g_assert_no_error (error);

  portal = xdp_portal_new ();

  uri = g_strconcat ("file://", path, NULL);

  got_info = 0;
  xdp_portal_set_wallpaper (portal, NULL, uri, target, NULL, wallpaper_cb, keyfile);

  while (!got_info)
    g_main_context_iteration (NULL, TRUE);
}
