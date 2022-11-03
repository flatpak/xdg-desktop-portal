#include "config.h"

#include <glib.h>

#include "xdp-utils.h"

static void
test_parse_cgroup_unified (void)
{
  char data[] = "0::/user.slice/user-1000.slice/user@1000.service/apps.slice/snap.something.scope\n";
  FILE *f;
  int res;
  gboolean is_snap = FALSE;

  f = fmemopen(data, sizeof(data), "r");

  res = _xdp_parse_cgroup_file (f, &is_snap);
  g_assert_cmpint (res, ==, 0);
  g_assert_true (is_snap);
  fclose(f);
}

static void
test_parse_cgroup_freezer (void)
{
  char data[] =
    "12:pids:/user.slice/user-1000.slice/user@1000.service\n"
    "11:perf_event:/\n"
    "10:net_cls,net_prio:/\n"
    "9:cpuset:/\n"
    "8:memory:/user.slice/user-1000.slice/user@1000.service/apps.slice/apps-org.gnome.Terminal.slice/vte-spawn-228ae109-a869-4533-8988-65ea4c10b492.scope\n"
    "7:rdma:/\n"
    "6:devices:/user.slice\n"
    "5:blkio:/user.slice\n"
    "4:hugetlb:/\n"
    "3:freezer:/snap.portal-test\n"
    "2:cpu,cpuacct:/user.slice\n"
    "1:name=systemd:/user.slice/user-1000.slice/user@1000.service/apps.slice/apps-org.gnome.Terminal.slice/vte-spawn-228ae109-a869-4533-8988-65ea4c10b492.scope\n"
    "0::/user.slice/user-1000.slice/user@1000.service/apps.slice/apps-org.gnome.Terminal.slice/vte-spawn-228ae109-a869-4533-8988-65ea4c10b492.scope\n";
  FILE *f;
  int res;
  gboolean is_snap = FALSE;

  f = fmemopen(data, sizeof(data), "r");

  res = _xdp_parse_cgroup_file (f, &is_snap);
  g_assert_cmpint (res, ==, 0);
  g_assert_true (is_snap);
  fclose(f);
}

static void
test_parse_cgroup_systemd (void)
{
  char data[] = "1:name=systemd:/user.slice/user-1000.slice/user@1000.service/apps.slice/snap.something.scope\n";
  FILE *f;
  int res;
  gboolean is_snap = FALSE;

  f = fmemopen(data, sizeof(data), "r");

  res = _xdp_parse_cgroup_file (f, &is_snap);
  g_assert_cmpint (res, ==, 0);
  g_assert_true (is_snap);
  fclose(f);
}

static void
test_parse_cgroup_not_snap (void)
{
  char data[] =
    "12:pids:/\n"
    "11:perf_event:/\n"
    "10:net_cls,net_prio:/\n"
    "9:cpuset:/\n"
    "8:memory:/\n"
    "7:rdma:/\n"
    "6:devices:/\n"
    "5:blkio:/\n"
    "4:hugetlb:/\n"
    "3:freezer:/\n"
    "2:cpu,cpuacct:/\n"
    "1:name=systemd:/\n"
    "0::/\n";

  FILE *f;
  int res;
  gboolean is_snap = FALSE;

  f = fmemopen(data, sizeof(data), "r");

  res = _xdp_parse_cgroup_file (f, &is_snap);
  g_assert_cmpint (res, ==, 0);
  g_assert_false (is_snap);
  fclose(f);
}

static void
test_alternate_doc_path (void)
{
  g_autofree char *path = NULL;

  xdp_set_documents_mountpoint (NULL);

  /* If no documents mount point is set, there is no alternate path */
  path = xdp_get_alternate_document_path ("/whatever", "app-id", XDP_APP_INFO_KIND_HOST);
  g_assert_cmpstr (path, ==, NULL);

  xdp_set_documents_mountpoint ("/doc/portal");

  /* Paths outside of the document portal do not have an alternate path */
  path = xdp_get_alternate_document_path ("/whatever", "app-id", XDP_APP_INFO_KIND_HOST);
  g_assert_cmpstr (path, ==, NULL);

  /* The doc portal mount point itself does not have an alternate path */
  path = xdp_get_alternate_document_path ("/doc/portal", "app-id", XDP_APP_INFO_KIND_HOST);
  g_assert_cmpstr (path, ==, NULL);

  /* Paths under the doc portal mount point have an alternate path */
  path = xdp_get_alternate_document_path ("/doc/portal/foo/bar", "app-id", XDP_APP_INFO_KIND_HOST);
  g_assert_cmpstr (path, ==, "/doc/portal/by-app/app-id/foo/bar");

  g_clear_pointer (&path, g_free);
  path = xdp_get_alternate_document_path ("/doc/portal/foo/bar", "second-app", XDP_APP_INFO_KIND_HOST);
  g_assert_cmpstr (path, ==, "/doc/portal/by-app/second-app/foo/bar");

  xdp_set_documents_mountpoint (NULL);
}

#ifdef HAVE_LIBSYSTEMD
static void
test_app_id_via_systemd_unit (void)
{
  g_autofree char *app_id = NULL;

  app_id = _xdp_parse_app_id_from_unit_name ("app-not-a-well-formed-unit-name");
  g_assert_cmpstr (app_id, ==, "");
  g_clear_pointer (&app_id, g_free);

  app_id = _xdp_parse_app_id_from_unit_name ("app-gnome-org.gnome.Evolution\\x2dalarm\\x2dnotify-2437.scope");
  /* Note, this is not Evolution's app ID, because the scope is for a background service */
  g_assert_cmpstr (app_id, ==, "org.gnome.Evolution-alarm-notify");
  g_clear_pointer (&app_id, g_free);

  app_id = _xdp_parse_app_id_from_unit_name ("app-gnome-org.gnome.Epiphany-182352.scope");
  g_assert_cmpstr (app_id, ==, "org.gnome.Epiphany");
  g_clear_pointer (&app_id, g_free);

  app_id = _xdp_parse_app_id_from_unit_name ("app-glib-spice\\x2dvdagent-1839.scope");
  /* App IDs must have two periods */
  g_assert_cmpstr (app_id, ==, "");
  g_clear_pointer (&app_id, g_free);

  app_id = _xdp_parse_app_id_from_unit_name ("app-KDE-org.kde.okular@12345.service");
  g_assert_cmpstr (app_id, ==, "org.kde.okular");
  g_clear_pointer (&app_id, g_free);

  app_id = _xdp_parse_app_id_from_unit_name ("app-org.kde.amarok.service");
  g_assert_cmpstr (app_id, ==, "org.kde.amarok");
  g_clear_pointer (&app_id, g_free);

  app_id = _xdp_parse_app_id_from_unit_name ("app-gnome-org.gnome.SettingsDaemon.DiskUtilityNotify-autostart.service");
  g_assert_cmpstr (app_id, ==, "org.gnome.SettingsDaemon.DiskUtilityNotify");
  g_clear_pointer (&app_id, g_free);

  app_id = _xdp_parse_app_id_from_unit_name ("app-gnome-org.gnome.Terminal-92502.slice");
  g_assert_cmpstr (app_id, ==, "org.gnome.Terminal");
  g_clear_pointer (&app_id, g_free);

  app_id = _xdp_parse_app_id_from_unit_name ("app-com.obsproject.Studio-d70acc38b5154a3a8b4a60accc4b15f4.scope");
  g_assert_cmpstr (app_id, ==, "com.obsproject.Studio");
  g_clear_pointer (&app_id, g_free);
}
#endif /* HAVE_LIBSYSTEMD */

int main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/parse-cgroup/unified", test_parse_cgroup_unified);
  g_test_add_func ("/parse-cgroup/freezer", test_parse_cgroup_freezer);
  g_test_add_func ("/parse-cgroup/systemd", test_parse_cgroup_systemd);
  g_test_add_func ("/parse-cgroup/not-snap", test_parse_cgroup_not_snap);
  g_test_add_func ("/alternate-doc-path", test_alternate_doc_path);
#ifdef HAVE_LIBSYSTEMD
  g_test_add_func ("/app-id-via-systemd-unit", test_app_id_via_systemd_unit);
#endif
  return g_test_run ();
}
