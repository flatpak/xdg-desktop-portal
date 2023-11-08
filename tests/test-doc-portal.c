#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib/gstdio.h>

#include "document-portal/document-portal-dbus.h"

#include "can-use-fuse.h"
#include "src/glib-backports.h"
#include "utils.h"

char outdir[] = "/tmp/xdp-test-XXXXXX";

char fuse_status_file[] = "/tmp/test-xdp-fuse-XXXXXX";

GTestDBus *dbus;
GDBusConnection *session_bus;
XdpDbusDocuments *documents;
char *mountpoint;

static gboolean
set_contents_trunc (const gchar  *filename,
                    const gchar  *contents,
                    gssize	   length,
                    GError	 **error)
{
  int fd;

  if (length == -1)
    length = strlen (contents);

  fd = open (filename, O_RDWR | O_TRUNC | O_CREAT, 0644);
  if (fd == -1)
    {
      int errsv = errno;
      g_set_error (error,
                   G_FILE_ERROR,
                   g_file_error_from_errno (errsv),
                   "Can't open %s", filename);
      return FALSE;
    }

  while (length > 0)
    {
      gssize s;

      s = write (fd, contents, length);

      if (s < 0)
        {
          int errsv = errno;
          if (errsv == EINTR)
            continue;

          g_set_error (error,
                       G_FILE_ERROR,
                       g_file_error_from_errno (errsv),
                       "Can't write to %s", filename);
          close (fd);
          return FALSE;
        }

      contents += s;
      length -= s;
    }

  close (fd);
  return TRUE;
}

static char *
make_doc_dir (const char *id, const char *app)
{
  if (app)
    return g_build_filename (mountpoint, "by-app", app, id, NULL);
  else
    return g_build_filename (mountpoint, id, NULL);
}

static char *
make_doc_path (const char *id, const char *basename, const char *app)
{
  g_autofree char *dir = make_doc_dir (id, app);

  return g_build_filename (dir, basename, NULL);
}

static void
assert_host_has_contents (const char *basename, const char *expected_contents)
{
  g_autofree char *path = g_build_filename (outdir, basename, NULL);
  g_autofree char *real_contents = NULL;
  gsize real_contents_length;
  GError *error = NULL;

  g_file_get_contents (path, &real_contents, &real_contents_length, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (real_contents, ==, expected_contents);
  g_assert_cmpuint (real_contents_length, ==, strlen (expected_contents));
}

static void
assert_doc_has_contents (const char *id, const char *basename, const char *app, const char *expected_contents)
{
  g_autofree char *path = make_doc_path (id, basename, app);
  g_autofree char *real_contents = NULL;
  gsize real_contents_length;
  GError *error = NULL;

  g_file_get_contents (path, &real_contents, &real_contents_length, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (real_contents, ==, expected_contents);
  g_assert_cmpuint (real_contents_length, ==, strlen (expected_contents));
}

static void
assert_doc_not_exist (const char *id, const char *basename, const char *app)
{
  g_autofree char *path = make_doc_path (id, basename, app);
  struct stat buf;
  int res, fd;

  res = stat (path, &buf);
  g_assert_cmpint (res, ==, -1);
  g_assert_cmpint (errno, ==, ENOENT);

  fd = open (path, O_RDONLY);
  g_assert_cmpint (fd, ==, -1);
  g_assert_cmpint (errno, ==, ENOENT);
}

static void
assert_doc_dir_not_exist (const char *id, const char *app)
{
  g_autofree char *path = make_doc_dir (id, app);
  struct stat buf;
  int res, fd;

  res = stat (path, &buf);
  g_assert_cmpint (res, ==, -1);
  g_assert_cmpint (errno, ==, ENOENT);

  fd = open (path, O_RDONLY);
  g_assert_cmpint (fd, ==, -1);
  g_assert_cmpint (errno, ==, ENOENT);
}

static void
assert_doc_dir_exist (const char *id, const char *app)
{
  g_autofree char *path = make_doc_dir (id, app);
  struct stat buf;
  int res, fd;

  res = stat (path, &buf);
  g_assert_cmpint (res, ==, 0);

  fd = open (path, O_RDONLY);
  g_assert_cmpint (fd, !=, -1);
  close (fd);
}

static char *
export_named_file (const char *dir, const char *name, gboolean unique)
{
  int fd, fd_id;
  GUnixFDList *fd_list = NULL;

  g_autoptr(GVariant) reply = NULL;
  GError *error = NULL;
  char *doc_id;

  fd = open (dir, O_PATH | O_CLOEXEC);
  g_assert (fd >= 0);

  fd_list = g_unix_fd_list_new ();
  fd_id = g_unix_fd_list_append (fd_list, fd, &error);
  g_assert_no_error (error);
  close (fd);

  reply = g_dbus_connection_call_with_unix_fd_list_sync (session_bus,
                                                         "org.freedesktop.portal.Documents",
                                                         "/org/freedesktop/portal/documents",
                                                         "org.freedesktop.portal.Documents",
                                                         "AddNamed",
                                                         g_variant_new ("(h^aybb)", fd_id, name, !unique, FALSE),
                                                         G_VARIANT_TYPE ("(s)"),
                                                         G_DBUS_CALL_FLAGS_NONE,
                                                         30000,
                                                         fd_list, NULL,
                                                         NULL,
                                                         &error);
  g_object_unref (fd_list);
  g_assert_no_error (error);
  g_assert (reply != NULL);

  g_variant_get (reply, "(s)", &doc_id);
  g_assert (doc_id != NULL);
  return doc_id;
}

static char *
export_file (const char *path, gboolean unique)
{
  int fd, fd_id;
  GUnixFDList *fd_list = NULL;

  g_autoptr(GVariant) reply = NULL;
  GError *error = NULL;
  char *doc_id;

  fd = open (path, O_PATH | O_CLOEXEC);
  g_assert (fd >= 0);

  fd_list = g_unix_fd_list_new ();
  fd_id = g_unix_fd_list_append (fd_list, fd, &error);
  g_assert_no_error (error);
  close (fd);

  reply = g_dbus_connection_call_with_unix_fd_list_sync (session_bus,
                                                         "org.freedesktop.portal.Documents",
                                                         "/org/freedesktop/portal/documents",
                                                         "org.freedesktop.portal.Documents",
                                                         "Add",
                                                         g_variant_new ("(hbb)", fd_id, !unique, FALSE),
                                                         G_VARIANT_TYPE ("(s)"),
                                                         G_DBUS_CALL_FLAGS_NONE,
                                                         30000,
                                                         fd_list, NULL,
                                                         NULL,
                                                         &error);
  g_object_unref (fd_list);
  g_assert_no_error (error);
  g_assert (reply != NULL);

  g_variant_get (reply, "(s)", &doc_id);
  g_assert (doc_id != NULL);
  return doc_id;
}

static char *
export_new_file (const char *basename, const char *contents, gboolean unique)
{
  g_autofree char *path = NULL;
  GError *error = NULL;

  path = g_build_filename (outdir, basename, NULL);

  g_file_set_contents (path, contents, -1, &error);
  g_assert_no_error (error);

  return export_file (path, unique);
}

static gboolean
update_doc_trunc (const char *id, const char *basename, const char *app, const char *contents, GError **error)
{
  g_autofree char *path = make_doc_path (id, basename, app);

  return set_contents_trunc (path, contents, -1, error);
}

static gboolean
update_doc (const char *id, const char *basename, const char *app, const char *contents, GError **error)
{
  g_autofree char *path = make_doc_path (id, basename, app);

  return g_file_set_contents (path, contents, -1, error);
}

static gboolean
update_from_host (const char *basename, const char *contents, GError **error)
{
  g_autofree char *path = g_build_filename (outdir, basename, NULL);

  return g_file_set_contents (path, contents, -1, error);
}

static gboolean
unlink_doc (const char *id, const char *basename, const char *app, GError **error)
{
  g_autofree char *path = make_doc_path (id, basename, app);

  if (unlink (path) != 0)
    {
      int errsv = errno;
      g_set_error (error,
                   G_FILE_ERROR,
                   g_file_error_from_errno (errsv),
                   "Can't unlink %s", path);
      return FALSE;
    }

  return TRUE;
}

static gboolean
unlink_doc_from_host (const char *basename, GError **error)
{
  g_autofree char *path = g_build_filename (outdir, basename, NULL);

  if (unlink (path) != 0)
    {
      int errsv = errno;
      g_set_error (error,
                   G_FILE_ERROR,
                   g_file_error_from_errno (errsv),
                   "Can't unlink %s", path);
      return FALSE;
    }

  return TRUE;
}

static void
grant_permissions (const char *id, const char *app, gboolean write)
{
  g_autoptr(GPtrArray) permissions = g_ptr_array_new ();
  GError *error = NULL;

  g_ptr_array_add (permissions, "read");
  if (write)
    g_ptr_array_add (permissions, "write");
  g_ptr_array_add (permissions, NULL);

  xdp_dbus_documents_call_grant_permissions_sync (documents,
                                                  id,
                                                  app,
                                                  (const char **) permissions->pdata,
                                                  NULL,
                                                  &error);
  g_assert_no_error (error);
}

static void
test_create_doc (void)
{
  g_autofree char *doc_path = NULL;
  g_autofree char *doc_app_path = NULL;
  g_autofree char *host_path = NULL;
  g_autofree char *id = NULL;
  g_autofree char *id2 = NULL;
  g_autofree char *id3 = NULL;
  g_autofree char *id4 = NULL;
  g_autofree char *id5 = NULL;
  const char *basename = "a-file";
  GError *error = NULL;

  if (!check_fuse_or_skip_test ())
    return;

  /* Export a document */
  id = export_new_file (basename, "content", FALSE);

  /* Ensure its there and not viewable by apps */
  assert_doc_has_contents (id, basename, NULL, "content");
  assert_host_has_contents (basename, "content");
  assert_doc_not_exist (id, basename, "com.test.App1");
  assert_doc_not_exist (id, basename, "com.test.App2");
  assert_doc_not_exist (id, "another-file", NULL);
  assert_doc_not_exist ("anotherid", basename, NULL);

  /* Create a tmp file in same dir, ensure it works and can't be seen by other apps */
  assert_doc_not_exist (id, "tmp1", NULL);
  update_doc (id, "tmp1", NULL, "tmpdata1", &error);
  g_assert_no_error (error);
  assert_doc_has_contents (id, "tmp1", NULL, "tmpdata1");
  assert_doc_not_exist (id, "tmp1", "com.test.App1");

  /* Let App 1 see the document (but not write) */
  grant_permissions (id, "com.test.App1", FALSE);

  /* Ensure App 1 and only it can see the document and tmpfile */
  assert_doc_has_contents (id, basename, "com.test.App1", "content");
  assert_doc_not_exist (id, basename, "com.test.App2");
  assert_doc_not_exist (id, "tmp1", "com.test.App1");

  /* Make sure App 1 can't create a tmpfile */
  assert_doc_not_exist (id, "tmp2", "com.test.App1");
  update_doc (id, "tmp2", "com.test.App1", "tmpdata2", &error);
  g_assert_error (error, G_FILE_ERROR, G_FILE_ERROR_ACCES);
  g_clear_error (&error);
  assert_doc_not_exist (id, "tmp2", "com.test.App1");

  /* Update the document contents, ensure this is propagated */
  update_doc (id, basename, NULL, "content2", &error);
  g_assert_no_error (error);

  assert_host_has_contents (basename, "content2");
  assert_doc_has_contents (id, basename, NULL, "content2");
  assert_doc_has_contents (id, basename, "com.test.App1", "content2");
  assert_doc_not_exist (id, basename, "com.test.App2");
  assert_doc_not_exist (id, "tmp1", "com.test.App2");

  /* Update the document contents outside fuse fd, ensure this is propagated */
  update_from_host (basename, "content3", &error);
  g_assert_no_error (error);
  assert_host_has_contents (basename, "content3");
  assert_doc_has_contents (id, basename, NULL, "content3");
  assert_doc_has_contents (id, basename, "com.test.App1", "content3");
  assert_doc_not_exist (id, basename, "com.test.App2");
  assert_doc_not_exist (id, "tmp1", "com.test.App2");

  /* Try to update the doc from an app that can't write to it */
  update_doc (id, basename, "com.test.App1", "content4", &error);
  g_assert_error (error, G_FILE_ERROR, G_FILE_ERROR_ACCES);
  g_clear_error (&error);

  /* Try to create a tmp file for an app that is not allowed */
  assert_doc_not_exist (id, "tmp2", "com.test.App1");
  update_doc (id, "tmp2", "com.test.App1", "tmpdata2", &error);
  g_assert_error (error, G_FILE_ERROR, G_FILE_ERROR_ACCES);
  g_clear_error (&error);
  assert_doc_not_exist (id, "tmp2", "com.test.App1");
  assert_doc_not_exist (id, "tmp2", NULL);

  /* Grant write permissions to App1 */
  grant_permissions (id, "com.test.App1", TRUE);

  /* update the doc from an app with write access */
  update_doc (id, basename, "com.test.App1", "content5", &error);
  g_assert_no_error (error);
  assert_host_has_contents (basename, "content5");
  assert_doc_has_contents (id, basename, NULL, "content5");
  assert_doc_has_contents (id, basename, "com.test.App1", "content5");
  assert_doc_not_exist (id, basename, "com.test.App2");

  /* Try to create a tmp file for an app */
  assert_doc_not_exist (id, "tmp3", "com.test.App1");
  update_doc (id, "tmp3", "com.test.App1", "tmpdata3", &error);
  g_assert_no_error (error);
  assert_doc_has_contents (id, "tmp3", "com.test.App1", "tmpdata3");
  assert_doc_not_exist (id, "tmp3", NULL);

  /* Re-Create a file from a fuse document file, in various ways */
  doc_path = make_doc_path (id, basename, NULL);
  doc_app_path = make_doc_path (id, basename, "com.test.App1");
  host_path = g_build_filename (outdir, basename, NULL);
  id2 = export_file (doc_path, FALSE);
  g_assert_cmpstr (id, ==, id2);
  id3 = export_file (doc_app_path, FALSE);
  g_assert_cmpstr (id, ==, id3);
  id4 = export_file (host_path, FALSE);
  g_assert_cmpstr (id, ==, id4);

  /* Ensure we can make a unique document */
  id5 = export_file (host_path, TRUE);
  g_assert_cmpstr (id, !=, id5);
}

static void
test_recursive_doc (void)
{
  g_autofree char *id = NULL;
  g_autofree char *id2 = NULL;
  g_autofree char *id3 = NULL;
  const char *basename = "recursive-file";
  g_autofree char *path = NULL;
  g_autofree char *app_path = NULL;

  if (!check_fuse_or_skip_test ())
    return;

  id = export_new_file (basename, "recursive-content", FALSE);

  assert_doc_has_contents (id, basename, NULL, "recursive-content");

  path = make_doc_path (id, basename, NULL);
  g_debug ("path: %s\n", path);

  id2 = export_file (path, FALSE);

  g_assert_cmpstr (id, ==, id2);

  grant_permissions (id, "com.test.App1", FALSE);

  app_path = make_doc_path (id, basename, "com.test.App1");

  id3 = export_file (app_path, FALSE);

  g_assert_cmpstr (id, ==, id3);
}

static void
test_create_docs (void)
{
  GError *error = NULL;
  g_autofree char *path1 = NULL;
  g_autofree char *path2 = NULL;
  int fd1, fd2;
  guint32 fd_ids[2];
  g_autoptr(GUnixFDList) fd_list = NULL;
  gboolean res;
  g_auto(GStrv) out_doc_ids = NULL;
  g_autoptr(GVariant) out_extra = NULL;
  const char *permissions[] = { "read", NULL };
  const char *basenames[] = { "doc1", "doc2" };
  int i;

  if (!check_fuse_or_skip_test ())
    return;

  path1 = g_build_filename (outdir, basenames[0], NULL);
  g_file_set_contents (path1, basenames[0], -1, &error);
  g_assert_no_error (error);

  fd1 = open (path1, O_PATH | O_CLOEXEC);
  g_assert (fd1 >= 0);

  path2 = g_build_filename (outdir, basenames[1], NULL);
  g_file_set_contents (path2, basenames[1], -1, &error);
  g_assert_no_error (error);

  fd2 = open (path2, O_PATH | O_CLOEXEC);
  g_assert (fd2 >= 0);

  fd_list = g_unix_fd_list_new ();
  fd_ids[0] = g_unix_fd_list_append (fd_list, fd1, &error);
  g_assert_no_error (error);
  close (fd1);
  fd_ids[1] = g_unix_fd_list_append (fd_list, fd2, &error);
  g_assert_no_error (error);
  close (fd2);

  res = xdp_dbus_documents_call_add_full_sync (documents,
                                               g_variant_new_fixed_array (G_VARIANT_TYPE_HANDLE,
                                                                          fd_ids, 2, sizeof (guint32)),
                                               0,
                                               "org.other.App",
                                               permissions,
                                               fd_list,
                                               &out_doc_ids,
                                               &out_extra,
                                               NULL,
                                               NULL, &error);
  g_assert_no_error (error);
  g_assert (res);

  g_assert (g_strv_length (out_doc_ids) == 2);
  for (i = 0; i < 2; i++)
    {
      const char *id = out_doc_ids[i];

      /* Ensure its there and not viewable by apps */
      assert_doc_has_contents (id, basenames[i], NULL, basenames[i]);
      assert_host_has_contents (basenames[i], basenames[i]);
      assert_doc_not_exist (id, basenames[i], "com.test.App1");
      assert_doc_not_exist (id, basenames[i], "com.test.App2");
      assert_doc_not_exist (id, "another-file", NULL);
      assert_doc_not_exist ("anotherid", basenames[i], NULL);

      assert_doc_has_contents (id, basenames[i], "org.other.App", basenames[i]);
      update_doc (id, basenames[i], "org.other.App", "tmpdata2", &error);
      g_assert_error (error, G_FILE_ERROR, G_FILE_ERROR_ACCES);
      g_clear_error (&error);
    }
  g_assert (g_variant_lookup_value (out_extra, "mountpoint", G_VARIANT_TYPE_VARIANT) == 0);
}


static void
test_add_named (void)
{
  g_autofree char *id1 = NULL;
  const char *basename1 = "add-named-1";
  GError *error = NULL;
  gboolean res;

  if (!check_fuse_or_skip_test ())
    return;

  id1 = export_named_file (outdir, basename1, FALSE);

  assert_doc_dir_exist (id1, NULL);
  assert_doc_dir_not_exist (id1, "com.test.App1");
  assert_doc_not_exist (id1, basename1, NULL);
  assert_doc_not_exist (id1, basename1,  "com.test.App1");

  grant_permissions (id1, "com.test.App1", TRUE);

  assert_doc_dir_exist (id1, NULL);
  assert_doc_dir_exist (id1, "com.test.App1");
  assert_doc_not_exist (id1, basename1, NULL);
  assert_doc_not_exist (id1, basename1,  "com.test.App1");

  /* Update truncating with no previous file */
  res = update_doc_trunc (id1, basename1, NULL, "foobar", &error);
  g_assert_no_error (error);
  g_assert (res == TRUE);

  assert_doc_has_contents (id1, basename1, NULL, "foobar");
  assert_doc_has_contents (id1, basename1, "com.test.App1", "foobar");
  assert_doc_not_exist (id1, basename1, "com.test.App2");

  /* Update truncating with previous file */
  res = update_doc_trunc (id1, basename1, NULL, "foobar2", &error);
  g_assert_no_error (error);
  g_assert (res == TRUE);

  assert_doc_has_contents (id1, basename1, NULL, "foobar2");
  assert_doc_has_contents (id1, basename1, "com.test.App1", "foobar2");
  assert_doc_not_exist (id1, basename1, "com.test.App2");

  /* Update atomic with previous file */
  res = update_doc (id1, basename1, NULL, "foobar3", &error);
  g_assert_no_error (error);
  g_assert (res == TRUE);

  assert_doc_has_contents (id1, basename1, NULL, "foobar3");
  assert_doc_has_contents (id1, basename1, "com.test.App1", "foobar3");
  assert_doc_not_exist (id1, basename1, "com.test.App2");

  /* Update from host */
  res = update_from_host (basename1, "foobar4", &error);
  g_assert_no_error (error);
  g_assert (res == TRUE);

  assert_doc_has_contents (id1, basename1, NULL, "foobar4");
  assert_doc_has_contents (id1, basename1, "com.test.App1", "foobar4");
  assert_doc_not_exist (id1, basename1, "com.test.App2");

  /* Unlink doc */
  res = unlink_doc (id1, basename1, NULL, &error);
  g_assert_no_error (error);
  g_assert (res == TRUE);

  assert_doc_dir_exist (id1, NULL);
  assert_doc_dir_exist (id1, "com.test.App1");
  assert_doc_not_exist (id1, basename1, NULL);
  assert_doc_not_exist (id1, basename1,  "com.test.App1");

  /* Update atomic with no previous file */
  res = update_doc (id1, basename1, NULL, "foobar5", &error);
  g_assert_no_error (error);
  g_assert (res == TRUE);

  assert_doc_has_contents (id1, basename1, NULL, "foobar5");
  assert_doc_has_contents (id1, basename1, "com.test.App1", "foobar5");
  assert_doc_not_exist (id1, basename1, "com.test.App2");

  /* Unlink doc on host */
  res = unlink_doc_from_host (basename1, &error);
  g_assert_no_error (error);
  g_assert (res == TRUE);

  assert_doc_dir_exist (id1, NULL);
  assert_doc_dir_exist (id1, "com.test.App1");
  assert_doc_not_exist (id1, basename1, NULL);
  assert_doc_not_exist (id1, basename1,  "com.test.App1");

  /* Update atomic with unexpected no previous file */
  res = update_doc (id1, basename1, NULL, "foobar6", &error);
  g_assert_no_error (error);
  g_assert (res == TRUE);

  assert_doc_has_contents (id1, basename1, NULL, "foobar6");
  assert_doc_has_contents (id1, basename1, "com.test.App1", "foobar6");
  assert_doc_not_exist (id1, basename1, "com.test.App2");

  /* Unlink doc on host again */
  res = unlink_doc_from_host (basename1, &error);
  g_assert_no_error (error);
  g_assert (res == TRUE);

  assert_doc_dir_exist (id1, NULL);
  assert_doc_dir_exist (id1, "com.test.App1");
  assert_doc_not_exist (id1, basename1, NULL);
  assert_doc_not_exist (id1, basename1,  "com.test.App1");

  /* Update truncating with unexpected no previous file */
  res = update_doc_trunc (id1, basename1, NULL, "foobar7", &error);
  g_assert_no_error (error);
  g_assert (res == TRUE);

  assert_doc_has_contents (id1, basename1, NULL, "foobar7");
  assert_doc_has_contents (id1, basename1, "com.test.App1", "foobar7");
  assert_doc_not_exist (id1, basename1, "com.test.App2");
}

static void
global_setup (void)
{
  gboolean inited;
  GError *error = NULL;
  g_autofree gchar *services = NULL;
  int fd;

  if (!check_fuse ())
    {
      g_assert_cmpstr (cannot_use_fuse, !=, NULL);
      return;
    }

  g_log_writer_default_set_use_stderr (TRUE);

  g_mkdtemp (outdir);
  g_debug ("outdir: %s\n", outdir);

  fd = g_mkstemp (fuse_status_file);
  close (fd);

  g_setenv ("XDG_RUNTIME_DIR", outdir, TRUE);
  g_setenv ("XDG_DATA_HOME", outdir, TRUE);
  g_setenv ("TEST_DOCUMENT_PORTAL_FUSE_STATUS", fuse_status_file, TRUE);

  /* Re-defining dbus-monitor with a custom script */
  setup_dbus_daemon_wrapper (outdir);

  dbus = g_test_dbus_new (G_TEST_DBUS_NONE);
  services = g_test_build_filename (G_TEST_BUILT, "services", NULL);
  g_test_dbus_add_service_dir (dbus, services);
  g_test_dbus_up (dbus);

  /* g_test_dbus_up unsets this, so re-set */
  g_setenv ("XDG_RUNTIME_DIR", outdir, TRUE);

  session_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
  g_assert_no_error (error);

  documents = xdp_dbus_documents_proxy_new_sync (session_bus, 0,
                                                 "org.freedesktop.portal.Documents",
                                                 "/org/freedesktop/portal/documents",
                                                 NULL, &error);
  g_assert_no_error (error);
  g_assert (documents != NULL);

  inited = xdp_dbus_documents_call_get_mount_point_sync (documents, &mountpoint,
                                                         NULL, &error);
  g_assert_no_error (error);
  g_assert (inited);
  g_assert (mountpoint != NULL);
}

static gboolean
rm_rf_dir (GFile         *dir,
           GError       **error)
{
  g_autoptr(GFileEnumerator) enumerator = NULL;
  g_autoptr(GFileInfo) child_info = NULL;
  GError *temp_error = NULL;

  enumerator = g_file_enumerate_children (dir, "standard::type,standard::name",
                                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                          NULL, error);
  if (!enumerator)
    return FALSE;

  while ((child_info = g_file_enumerator_next_file (enumerator, NULL, &temp_error)))
    {
      const char *name = g_file_info_get_name (child_info);
      g_autoptr(GFile) child = g_file_get_child (dir, name);

      if (g_file_info_get_file_type (child_info) == G_FILE_TYPE_DIRECTORY)
        {
          if (!rm_rf_dir (child, error))
            return FALSE;
        }
      else
        {
          if (!g_file_delete (child, NULL, error))
            return FALSE;
        }

      g_clear_object (&child_info);
    }

  if (temp_error != NULL)
    {
      g_propagate_error (error, temp_error);
      return FALSE;
    }

  if (!g_file_delete (dir, NULL, error))
    return FALSE;

  return TRUE;
}


static void
global_teardown (void)
{
  GError *error = NULL;
  char *argv[] = { "fusermount3", "-u", NULL, NULL };
  g_autofree char *by_app_dir = g_build_filename (mountpoint, "by-app", NULL);
  struct stat buf;
  g_autoptr(GFile) outdir_file = g_file_new_for_path (outdir);
  int res, i;

  if (cannot_use_fuse != NULL)
    return;

  res = stat (by_app_dir, &buf);
  g_assert_cmpint (res, ==, 0);

  argv[2] = mountpoint;

  g_spawn_sync (NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                NULL, NULL, NULL, NULL, NULL, &error);
  g_assert_no_error (error);

  res = stat (by_app_dir, &buf);
  g_assert_cmpint (res, ==, -1);
  g_assert_cmpint (errno, ==, ENOENT);

  for (i = 0; i < 1000; i++)
    {
      g_autofree char *fuse_unmount_status = NULL;

      g_file_get_contents (fuse_status_file, &fuse_unmount_status, NULL, &error);
      g_assert_no_error (error);
      /* Loop until something is written to the status file */
      if (strlen (fuse_unmount_status) > 0)
        {
          g_assert_cmpstr (fuse_unmount_status, ==, "ok");
          break;
        }
      g_usleep (G_USEC_PER_SEC / 100);
    }
  g_assert (i != 1000); /* We timed out before writing to the status file */
  (void) unlink (fuse_status_file);

  g_free (mountpoint);

  g_object_unref (documents);

  g_dbus_connection_close_sync (session_bus, NULL, &error);
  g_assert_no_error (error);

  g_object_unref (session_bus);

  g_test_dbus_down (dbus);

  g_object_unref (dbus);

  res = rm_rf_dir (outdir_file, &error);
  g_assert_no_error (error);
}

static void
test_version (void)
{
  if (!check_fuse_or_skip_test ())
    return;

  g_assert_cmpint (xdp_dbus_documents_get_version (documents), ==, 4);
}

int
main (int argc, char **argv)
{
  int res;

  /* Better leak reporting without gvfs */
  g_setenv ("GIO_USE_VFS", "local", TRUE);

  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/db/version", test_version);
  g_test_add_func ("/db/create_doc", test_create_doc);
  g_test_add_func ("/db/recursive_doc", test_recursive_doc);
  g_test_add_func ("/db/create_docs", test_create_docs);
  g_test_add_func ("/db/add_named", test_add_named);

  global_setup ();

  res = g_test_run ();

  global_teardown ();

  return res;
}
