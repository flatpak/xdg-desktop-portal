#include "config.h"

#include <glib.h>

#include "src/xdp-utils.h"

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

int main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/parse-cgroup/unified", test_parse_cgroup_unified);
  g_test_add_func ("/parse-cgroup/freezer", test_parse_cgroup_freezer);
  g_test_add_func ("/parse-cgroup/systemd", test_parse_cgroup_systemd);
  g_test_add_func ("/parse-cgroup/not-snap", test_parse_cgroup_not_snap);
  return g_test_run ();
}
