/* SPDX-License-Identifier: LGPL-2.1-or-later
 * SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
 */

#include "config.h"

#include <glib.h>

#include "xdp-app-info-flatpak-private.h"

#define APP_ID "org.example.Test"

static char *metadata_path;

static XdpAppInfo *
app_info_with_persistent (const char *persistent)
{
  g_autofree char *metadata = NULL;
  g_autoptr(GError) error = NULL;
  XdpAppInfo *app_info;

  metadata = g_strdup_printf (
    "[Application]\n"
    "name=" APP_ID "\n"
    "runtime=org.freedesktop.Platform/x86_64/24.08\n"
    "sdk=org.freedesktop.Sdk/x86_64/24.08\n"
    "command=" APP_ID "\n"
    "[Instance]\n"
    "instance-id=1234567890\n"
    "[Context]\n"
    "shared=network;ipc;\n"
    "sockets=wayland;pulseaudio;\n"
    "%s%s\n",
    persistent ? "persistent=" : "",
    persistent ? persistent : "");

  g_assert_true (g_file_set_contents (metadata_path, metadata, -1, NULL));

  app_info = xdp_app_info_flatpak_new (NULL, 0, NULL, &error);
  g_assert_no_error (error);
  g_assert_nonnull (app_info);

  return app_info;
}

static char *
remap (const char *persistent, const char *path)
{
  g_autoptr(XdpAppInfo) app_info = app_info_with_persistent (persistent);

  return xdp_app_info_remap_path (app_info, path);
}

static char *
in_home (const char *subpath)
{
  return g_build_filename (g_get_home_dir (), subpath, NULL);
}

static char *
in_app_dir (const char *subpath)
{
  return g_build_filename (g_get_home_dir (), ".var", "app", APP_ID,
                           subpath, NULL);
}

static void
test_persistent_basic (void)
{
  g_autofree char *path = in_home ("data");
  g_autofree char *expected = in_app_dir ("data");
  g_autofree char *result = remap ("data;", path);

  g_assert_cmpstr (result, ==, expected);
}

static void
test_persistent_subpath (void)
{
  g_autofree char *path = in_home ("data/sub/file.txt");
  g_autofree char *expected = in_app_dir ("data/sub/file.txt");
  g_autofree char *result = remap ("data;", path);

  g_assert_cmpstr (result, ==, expected);
}

static void
test_persistent_component_boundary (void)
{
  g_autofree char *path = in_home ("database");
  g_autofree char *result = remap ("data;", path);

  /* Not translated, so it must come back unchanged. */
  g_assert_cmpstr (result, ==, path);
}

static void
test_persistent_nested_entry (void)
{
  g_autofree char *path = in_home ("sub/dir/file.txt");
  g_autofree char *expected = in_app_dir ("sub/dir/file.txt");
  g_autofree char *result = remap ("sub/dir;", path);

  g_assert_cmpstr (result, ==, expected);
}

static void
test_persistent_dotdot_escape (void)
{
  g_autofree char *path = in_home ("data/../..");
  g_autofree char *result = remap ("data;", path);

  /* Escaping the per-app directory is refused, i.e. not translated. */
  g_assert_cmpstr (result, ==, path);
}

static void
test_persistent_absolute_entry (void)
{
  g_autofree char *path = in_home ("etc");
  g_autofree char *expected = in_app_dir ("data");
  g_autofree char *result = remap ("/etc;data;", path);
  g_autofree char *data_path = in_home ("data");
  g_autofree char *data_result = remap ("/etc;data;", data_path);

  g_assert_cmpstr (result, ==, path);
  g_assert_cmpstr (data_result, ==, expected);
}

static void
test_persistent_dotdot_entry (void)
{
  g_autofree char *path = in_home ("evil");
  g_autofree char *result = remap ("../evil;", path);

  g_assert_cmpstr (result, ==, path);
}

static void
test_persistent_empty (void)
{
  g_autofree char *path = in_home ("data");

  g_autofree char *empty_result = remap (";", path);
  g_autofree char *missing_result = remap (NULL, path);

  g_assert_cmpstr (empty_result, ==, path);
  g_assert_cmpstr (missing_result, ==, path);
}

static void
test_persistent_whole_home (void)
{
  g_autofree char *path = in_home ("foo/bar");
  g_autofree char *expected = in_app_dir ("foo/bar");
  g_autofree char *result = remap (".", path);

  g_assert_cmpstr (result, ==, expected);
}

static void
test_persistent_already_translated (void)
{
  g_autofree char *path = in_app_dir ("data");
  g_autofree char *result = remap ("data;", path);

  g_assert_cmpstr (result, ==, path);
}

int
main (int argc, char *argv[])
{
  g_autofree char *tmpdir = NULL;
  g_autofree char *data_dir = NULL;
  g_autofree char *apps_dir = NULL;
  g_autofree char *desktop_file = NULL;
  static const char desktop_entry[] =
    "[Desktop Entry]\n"
    "Type=Application\n"
    "Name=Example App\n"
    "Exec=true\n"
    "X-Flatpak=" APP_ID "\n";

  tmpdir = g_dir_make_tmp ("xdp-app-info-persist-XXXXXX", NULL);
  g_assert_nonnull (tmpdir);
  g_setenv ("HOME", tmpdir, TRUE);

  /* The app info requires a desktop file for the app id. */
  data_dir = g_build_filename (tmpdir, "data", NULL);
  apps_dir = g_build_filename (data_dir, "applications", NULL);
  g_assert_cmpint (g_mkdir_with_parents (apps_dir, 0755), ==, 0);
  desktop_file = g_build_filename (apps_dir, APP_ID ".desktop", NULL);
  g_assert_true (g_file_set_contents (desktop_file, desktop_entry, -1, NULL));
  g_setenv ("XDG_DATA_HOME", data_dir, TRUE);

  g_setenv ("XDG_DESKTOP_PORTAL_TEST_APP_INFO_KIND", "flatpak", TRUE);

  metadata_path = g_build_filename (tmpdir, "metadata", NULL);
  g_setenv ("XDG_DESKTOP_PORTAL_TEST_FLATPAK_METADATA", metadata_path, TRUE);

  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/app-info/persist/basic", test_persistent_basic);
  g_test_add_func ("/app-info/persist/subpath", test_persistent_subpath);
  g_test_add_func ("/app-info/persist/component-boundary",
                   test_persistent_component_boundary);
  g_test_add_func ("/app-info/persist/nested-entry",
                   test_persistent_nested_entry);
  g_test_add_func ("/app-info/persist/dotdot-escape",
                   test_persistent_dotdot_escape);
  g_test_add_func ("/app-info/persist/absolute-entry",
                   test_persistent_absolute_entry);
  g_test_add_func ("/app-info/persist/dotdot-entry", test_persistent_dotdot_entry);
  g_test_add_func ("/app-info/persist/empty", test_persistent_empty);
  g_test_add_func ("/app-info/persist/whole-home",
                   test_persistent_whole_home);
  g_test_add_func ("/app-info/persist/already-translated",
                   test_persistent_already_translated);

  return g_test_run ();
}
