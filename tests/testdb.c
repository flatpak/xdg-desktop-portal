#include "config.h"

#include <glib.h>
#include <document-portal/permission-db.h>

/*
static void
dump_db (PermissionDb *db)
{
  g_autofree char *s = permission_db_print (db);
  g_printerr ("\n%s\n", s);
}
*/

static PermissionDb *
create_test_db (gboolean serialized)
{
  PermissionDb *db;

  g_autoptr(PermissionDbEntry) entry1 = NULL;
  g_autoptr(PermissionDbEntry) entry2 = NULL;
  g_autoptr(PermissionDbEntry) entry3 = NULL;
  g_autoptr(PermissionDbEntry) entry4 = NULL;
  g_autoptr(PermissionDbEntry) entry5 = NULL;
  g_autoptr(PermissionDbEntry) entry6 = NULL;
  g_autoptr(PermissionDbEntry) entry7 = NULL;
  GError *error = NULL;
  const char *permissions1[] = { "read", "write", NULL };
  const char *permissions2[] = { "read", NULL };
  const char *permissions3[] = { "write", NULL };

  db = permission_db_new (NULL, FALSE, &error);
  g_assert_no_error (error);
  g_assert (db != NULL);

  {
    g_auto(GStrv) ids = permission_db_list_ids (db);
    g_assert (ids != NULL);
    g_assert (ids[0] == NULL);
  }

  {
    g_auto(GStrv) apps = permission_db_list_apps (db);
    g_assert (apps != NULL);
    g_assert (apps[0] == NULL);
  }

  entry1 = permission_db_entry_new (g_variant_new_string ("foo-data"));
  entry2 = permission_db_entry_set_app_permissions (entry1, "org.test.bapp", permissions2);
  entry3 = permission_db_entry_set_app_permissions (entry2, "org.test.app", permissions1);
  entry4 = permission_db_entry_set_app_permissions (entry3, "org.test.capp", permissions1);

  permission_db_set_entry (db, "foo", entry4);

  entry5 = permission_db_entry_new (g_variant_new_string ("bar-data"));
  entry6 = permission_db_entry_set_app_permissions (entry5, "org.test.app", permissions2);
  entry7 = permission_db_entry_set_app_permissions (entry6, "org.test.dapp", permissions3);

  permission_db_set_entry (db, "bar", entry7);

  if (serialized)
    permission_db_update (db);

  return db;
}

static void
verify_test_db (PermissionDb *db)
{
  g_auto(GStrv) ids = NULL;
  g_autofree const char **apps1 = NULL;
  g_autofree const char **apps2 = NULL;
  g_auto(GStrv) all_apps = NULL;

  ids = permission_db_list_ids (db);
  g_assert (g_strv_length (ids) == 2);
  g_assert (g_strv_contains ((const char **) ids, "foo"));
  g_assert (g_strv_contains ((const char **) ids, "bar"));

  {
    g_autoptr(PermissionDbEntry) entry = NULL;
    g_autofree const char **permissions1 = NULL;
    g_autofree const char **permissions2 = NULL;
    g_autofree const char **permissions3 = NULL;
    g_autofree const char **permissions4 = NULL;
    g_autoptr(GVariant) data1 = NULL;

    entry = permission_db_lookup (db, "foo");
    g_assert (entry != NULL);
    data1 = permission_db_entry_get_data (entry);
    g_assert (data1 != NULL);
    g_assert_cmpstr (g_variant_get_type_string (data1), ==, "s");
    g_assert_cmpstr (g_variant_get_string (data1, NULL), ==, "foo-data");
    apps1 = permission_db_entry_list_apps (entry);
    g_assert (g_strv_length ((char **) apps1) == 3);
    g_assert (g_strv_contains (apps1, "org.test.app"));
    g_assert (g_strv_contains (apps1, "org.test.bapp"));
    g_assert (g_strv_contains (apps1, "org.test.capp"));
    permissions1 = permission_db_entry_list_permissions (entry, "org.test.app");
    g_assert (g_strv_length ((char **) permissions1) == 2);
    g_assert (g_strv_contains (permissions1, "read"));
    g_assert (g_strv_contains (permissions1, "write"));
    permissions2 = permission_db_entry_list_permissions (entry, "org.test.bapp");
    g_assert (g_strv_length ((char **) permissions2) == 1);
    g_assert (g_strv_contains (permissions2, "read"));
    permissions3 = permission_db_entry_list_permissions (entry, "org.test.capp");
    g_assert (g_strv_length ((char **) permissions3) == 2);
    g_assert (g_strv_contains (permissions3, "read"));
    g_assert (g_strv_contains (permissions3, "write"));
    permissions4 = permission_db_entry_list_permissions (entry, "org.test.noapp");
    g_assert (permissions4 != NULL);
    g_assert (g_strv_length ((char **) permissions4) == 0);
  }

  {
    g_autoptr(PermissionDbEntry) entry = NULL;
    g_autofree const char **permissions5 = NULL;
    g_autofree const char **permissions6 = NULL;
    g_autoptr(GVariant) data2 = NULL;

    entry = permission_db_lookup (db, "bar");
    g_assert (entry != NULL);
    data2 = permission_db_entry_get_data (entry);
    g_assert (data2 != NULL);
    g_assert_cmpstr (g_variant_get_type_string (data2), ==, "s");
    g_assert_cmpstr (g_variant_get_string (data2, NULL), ==, "bar-data");
    apps2 = permission_db_entry_list_apps (entry);
    g_assert (g_strv_length ((char **) apps2) == 2);
    g_assert (g_strv_contains (apps2, "org.test.app"));
    g_assert (g_strv_contains (apps2, "org.test.dapp"));
    permissions5 = permission_db_entry_list_permissions (entry, "org.test.app");
    g_assert (g_strv_length ((char **) permissions5) == 1);
    g_assert (g_strv_contains (permissions5, "read"));
    permissions6 = permission_db_entry_list_permissions (entry, "org.test.dapp");
    g_assert (g_strv_length ((char **) permissions6) == 1);
    g_assert (g_strv_contains (permissions6, "write"));
  }

  {
    g_autoptr(PermissionDbEntry) entry = NULL;
    entry = permission_db_lookup (db, "gazonk");
    g_assert (entry == NULL);
  }

  all_apps = permission_db_list_apps (db);
  g_assert (g_strv_length (all_apps) == 4);
  g_assert (g_strv_contains ((const char **) all_apps, "org.test.app"));
  g_assert (g_strv_contains ((const char **) all_apps, "org.test.bapp"));
  g_assert (g_strv_contains ((const char **) all_apps, "org.test.capp"));
  g_assert (g_strv_contains ((const char **) all_apps, "org.test.dapp"));
}

static void
test_db_open (void)
{
  GError *error = NULL;
  PermissionDb *db;

  db = permission_db_new (g_test_get_filename (G_TEST_DIST, "dbs", "does_not_exist", NULL), TRUE, &error);
  g_assert (g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT) ||
            g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND));
  g_assert (db == NULL);
  g_clear_error (&error);

  db = permission_db_new (g_test_get_filename (G_TEST_DIST, "dbs", "does_not_exist", NULL), FALSE, &error);
  g_assert_no_error (error);
  g_assert (db != NULL);
  g_clear_error (&error);
  g_object_unref (db);

  db = permission_db_new (g_test_get_filename (G_TEST_DIST, "dbs", "no_tables", NULL), TRUE, &error);
  g_assert_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL);
  g_assert (db == NULL);
  g_clear_error (&error);
}

static void
test_serialize (void)
{
  g_autoptr(PermissionDb) db = NULL;
  g_autoptr(PermissionDb) db2 = NULL;
  g_autofree char *dump1 = NULL;
  g_autofree char *dump2 = NULL;
  g_autofree char *dump3 = NULL;
  GError *error = NULL;
  char tmpfile[] = "/tmp/testdbXXXXXX";
  int fd;

  db = create_test_db (FALSE);

  verify_test_db (db);

  dump1 = permission_db_print (db);

  g_assert (permission_db_is_dirty (db));

  permission_db_update (db);

  verify_test_db (db);

  g_assert (!permission_db_is_dirty (db));

  dump2 = permission_db_print (db);

  g_assert_cmpstr (dump1, ==, dump2);

  fd = g_mkstemp (tmpfile);
  close (fd);

  permission_db_set_path (db, tmpfile);

  permission_db_save_content (db, &error);
  g_assert_no_error (error);

  db2 = permission_db_new (tmpfile, TRUE, &error);
  g_assert_no_error (error);
  g_assert (db2 != NULL);

  dump3 = permission_db_print (db2);

  g_assert_cmpstr (dump1, ==, dump3);

  unlink (tmpfile);
}

static void
test_modify (void)
{
  g_autoptr(PermissionDb) db = NULL;
  const char *permissions[] = { "read", "write", "execute", NULL };
  const char *no_permissions[] = { NULL };

  db = create_test_db (FALSE);

  /* Add permission */
  {
    g_autoptr(PermissionDbEntry) entry1 = NULL;
    g_autoptr(PermissionDbEntry) entry2 = NULL;

    entry1 = permission_db_lookup (db, "foo");
    entry2 = permission_db_entry_set_app_permissions (entry1, "org.test.app", permissions);
    permission_db_set_entry (db, "foo", entry2);
  }

  /* Add entry */
  {
    g_autoptr(PermissionDbEntry) entry1 = NULL;
    g_autoptr(PermissionDbEntry) entry2 = NULL;

    entry1 = permission_db_entry_new (g_variant_new_string ("gazonk-data"));
    entry2 = permission_db_entry_set_app_permissions (entry1, "org.test.eapp", permissions);
    permission_db_set_entry (db, "gazonk", entry2);
  }

  /* Remove permission */
  {
    g_autoptr(PermissionDbEntry) entry1 = NULL;
    g_autoptr(PermissionDbEntry) entry2 = NULL;

    entry1 = permission_db_lookup (db, "bar");
    entry2 = permission_db_entry_set_app_permissions (entry1, "org.test.dapp", no_permissions);
    permission_db_set_entry (db, "bar", entry2);
  }

  /* Verify */
  {
    g_autoptr(PermissionDbEntry) entry5 = NULL;
    g_autoptr(PermissionDbEntry) entry6 = NULL;
    g_autoptr(PermissionDbEntry) entry7 = NULL;
    g_autofree const char **apps2 = NULL;
    g_auto(GStrv) apps3 = NULL;
    g_autofree const char **permissions1 = NULL;
    g_autofree const char **permissions2 = NULL;
    g_autofree const char **permissions3 = NULL;

    entry5 = permission_db_lookup (db, "foo");
    permissions1 = permission_db_entry_list_permissions (entry5, "org.test.app");
    g_assert (g_strv_length ((char **) permissions1) == 3);
    g_assert (g_strv_contains (permissions1, "read"));
    g_assert (g_strv_contains (permissions1, "write"));
    g_assert (g_strv_contains (permissions1, "execute"));

    entry6 = permission_db_lookup (db, "bar");
    permissions2 = permission_db_entry_list_permissions (entry6, "org.test.dapp");
    g_assert (g_strv_length ((char **) permissions2) == 0);

    entry7 = permission_db_lookup (db, "gazonk");
    permissions3 = permission_db_entry_list_permissions (entry7, "org.test.eapp");
    g_assert (g_strv_length ((char **) permissions3) == 3);
    g_assert (g_strv_contains (permissions3, "read"));
    g_assert (g_strv_contains (permissions3, "write"));
    g_assert (g_strv_contains (permissions3, "execute"));

    apps2 = permission_db_entry_list_apps (entry6);
    g_assert_cmpint (g_strv_length ((char **) apps2), ==, 1);
    g_assert (g_strv_contains (apps2, "org.test.app"));

    apps3 = permission_db_list_apps (db);
    g_assert_cmpint (g_strv_length (apps3), ==, 4);
    g_assert (g_strv_contains ((const char **) apps3, "org.test.app"));
    g_assert (g_strv_contains ((const char **) apps3, "org.test.bapp"));
    g_assert (g_strv_contains ((const char **) apps3, "org.test.capp"));
    g_assert (g_strv_contains ((const char **) apps3, "org.test.eapp"));
  }

  permission_db_update (db);

  /* Verify after serialize */
  {
    g_autoptr(PermissionDbEntry) entry5 = NULL;
    g_autoptr(PermissionDbEntry) entry6 = NULL;
    g_autoptr(PermissionDbEntry) entry7 = NULL;
    g_autofree const char **apps2 = NULL;
    g_auto(GStrv) apps3 = NULL;
    g_autofree const char **permissions1 = NULL;
    g_autofree const char **permissions2 = NULL;
    g_autofree const char **permissions3 = NULL;

    entry5 = permission_db_lookup (db, "foo");
    permissions1 = permission_db_entry_list_permissions (entry5, "org.test.app");
    g_assert (g_strv_length ((char **) permissions1) == 3);
    g_assert (g_strv_contains (permissions1, "read"));
    g_assert (g_strv_contains (permissions1, "write"));
    g_assert (g_strv_contains (permissions1, "execute"));

    entry6 = permission_db_lookup (db, "bar");
    permissions2 = permission_db_entry_list_permissions (entry6, "org.test.dapp");
    g_assert (g_strv_length ((char **) permissions2) == 0);

    entry7 = permission_db_lookup (db, "gazonk");
    permissions3 = permission_db_entry_list_permissions (entry7, "org.test.eapp");
    g_assert (g_strv_length ((char **) permissions3) == 3);
    g_assert (g_strv_contains (permissions3, "read"));
    g_assert (g_strv_contains (permissions3, "write"));
    g_assert (g_strv_contains (permissions3, "execute"));

    apps2 = permission_db_entry_list_apps (entry6);
    g_assert_cmpint (g_strv_length ((char **) apps2), ==, 1);
    g_assert (g_strv_contains (apps2, "org.test.app"));

    apps3 = permission_db_list_apps (db);
    g_assert_cmpint (g_strv_length (apps3), ==, 4);
    g_assert (g_strv_contains ((const char **) apps3, "org.test.app"));
    g_assert (g_strv_contains ((const char **) apps3, "org.test.bapp"));
    g_assert (g_strv_contains ((const char **) apps3, "org.test.capp"));
    g_assert (g_strv_contains ((const char **) apps3, "org.test.eapp"));
  }
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/db/open", test_db_open);
  g_test_add_func ("/db/serialize", test_serialize);
  g_test_add_func ("/db/modify", test_modify);

  return g_test_run ();
}
