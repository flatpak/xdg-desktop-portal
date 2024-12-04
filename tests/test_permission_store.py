# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests as xdp

import dbus
from gi.repository import GLib, Gio


class PermissionStore(xdp.GDBusIface):
    def __init__(self):
        super().__init__(
            "org.freedesktop.impl.portal.PermissionStore",
            "/org/freedesktop/impl/portal/PermissionStore",
            "org.freedesktop.impl.portal.PermissionStore",
        )

    def Lookup(self, table, id):
        return self._call(
            "Lookup",
            GLib.Variant("(ss)", (table, id)),
        )

    def Set(self, table, create, id, perm, data):
        return self._call(
            "Set",
            GLib.Variant("(sbsa{sas}v)", (table, create, id, perm, data)),
        )

    def SetValue(self, table, create, id, data):
        return self._call(
            "SetValue",
            GLib.Variant("(sbsv)", (table, create, id, data)),
        )

    def SetPermission(self, table, create, id, app, perm):
        return self._call(
            "SetPermission",
            GLib.Variant("(sbssas)", (table, create, id, app, perm)),
        )

    def SetPermissionAsync(self, table, create, id, app, perm, user_cb):
        self._call_async(
            "SetPermission",
            GLib.Variant("(sbssas)", (table, create, id, app, perm)),
            cb=user_cb,
        )

    def DeletePermissionAsync(self, table, id, app, user_cb):
        self._call_async(
            "DeletePermission",
            GLib.Variant("(sss)", (table, id, app)),
            cb=user_cb,
        )

    def DeleteAsync(self, table, id, user_cb):
        self._call_async(
            "Delete",
            GLib.Variant("(ss)", (table, id)),
            cb=user_cb,
        )

    def Delete(self, table, id):
        return self._call(
            "Delete",
            GLib.Variant("(ss)", (table, id)),
        )

    def GetPermission(self, table, id, app):
        return self._call(
            "GetPermission",
            GLib.Variant("(sss)", (table, id, app)),
        )


class TestPermissionStore:
    def test_version(self, portals, dbus_con):
        permission_store = dbus_con.get_object(
            "org.freedesktop.impl.portal.PermissionStore",
            "/org/freedesktop/impl/portal/PermissionStore",
        )

        properties_intf = dbus.Interface(
            permission_store,
            "org.freedesktop.DBus.Properties",
        )
        portal_version = properties_intf.Get(
            "org.freedesktop.impl.portal.PermissionStore",
            "version",
        )
        assert int(portal_version) == 2

    def test_delete_race(self, portals, dbus_con):
        permission_store_intf = PermissionStore()
        finished_count = 0

        table = "inhibit"
        id = "inhibit"
        perms = ["logout", "suspend"]

        def cb(_):
            nonlocal finished_count

            finished_count += 1

        permission_store_intf.SetPermissionAsync(table, True, id, "a", perms, cb)
        permission_store_intf.DeleteAsync(table, id, cb)

        xdp.wait_for(lambda: finished_count >= 2)

        try:
            permission_store_intf.Lookup(table, id)
            assert False, "This statement should not be reached"
        except GLib.GError as e:
            assert "org.freedesktop.portal.Error.NotFound" in e.message
            assert e.matches(Gio.io_error_quark(), Gio.IOErrorEnum.DBUS_ERROR)

        permission_store_intf.SetPermissionAsync(table, True, id, "a", perms, cb)
        permission_store_intf.SetPermissionAsync(table, True, id, "b", perms, cb)
        permission_store_intf.DeletePermissionAsync(table, id, "a", cb)

        xdp.wait_for(lambda: finished_count >= 4)

        result, _ = permission_store_intf.Lookup(table, id)
        perms_out = result.unpack()[0]
        assert perms_out == {"b": perms}

        permission_store_intf.SetPermissionAsync(table, True, id, "a", perms, cb)
        permission_store_intf.DeletePermissionAsync(table, id, "b", cb)
        permission_store_intf.DeletePermissionAsync(table, id, "a", cb)

        xdp.wait_for(lambda: finished_count >= 7)

        result, _ = permission_store_intf.Lookup(table, id)
        perms_out = result.unpack()[0]
        assert perms_out == {}

    def test_change(self, portals, dbus_con):
        permission_store_intf = PermissionStore()
        changed_count = 0

        table = "TEST"
        id = "test-resource"
        app = "one.two.three"
        perms = ["one", "two"]

        def cb_changed1(results):
            nonlocal changed_count

            cb_table, cb_id, deleted, _, cb_perms = results.unpack()

            assert cb_table == table
            assert cb_id == id
            assert not deleted
            assert cb_perms[app] == perms

            changed_count += 1

        cs = permission_store_intf.connect_to_signal("Changed", cb_changed1)
        permission_store_intf.SetPermissionAsync(table, True, id, app, perms, None)
        xdp.wait_for(lambda: changed_count >= 1)
        cs.disconnect()

        def cb_changed2(results):
            nonlocal changed_count

            cb_table, cb_id, deleted, _, _ = results.unpack()

            assert cb_table == table
            assert cb_id == id
            assert deleted

            changed_count += 1

        cs = permission_store_intf.connect_to_signal("Changed", cb_changed2)
        permission_store_intf.Delete(table, id)
        xdp.wait_for(lambda: changed_count >= 2)
        cs.disconnect()

    def test_lookup(self, portals, dbus_con):
        permission_store_intf = PermissionStore()

        table = "TEST"
        id = "test-resource"
        perms = ["one", "two"]
        data = True

        try:
            permission_store_intf.Lookup(table, id)
            assert False, "This statement should not be reached"
        except GLib.GError as e:
            assert "org.freedesktop.portal.Error.NotFound" in e.message

        permissions = [(id, perms)]
        permission_store_intf.Set(table, True, id, permissions, GLib.Variant("b", data))

        result, _ = permission_store_intf.Lookup(table, id)
        perms_out = result.unpack()[0]
        data_out = result.unpack()[1]

        assert id in perms_out
        perms_out = perms_out[id]
        assert perms_out == perms

        assert data_out == data

    def test_set_value(self, portals, dbus_con):
        permission_store_intf = PermissionStore()

        table = "TEST"
        id = "test-resource"
        data = True

        try:
            permission_store_intf.Lookup(table, id)
            assert False, "This statement should not be reached"
        except GLib.GError as e:
            assert "org.freedesktop.portal.Error.NotFound" in e.message

        permission_store_intf.SetValue(table, True, id, GLib.Variant("b", data))

        result, _ = permission_store_intf.Lookup(table, id)
        perms_out = result.unpack()[0]
        data_out = result.unpack()[1]
        assert perms_out == {}
        assert data_out == data

    def test_create(self, portals, dbus_con):
        permission_store_intf = PermissionStore()

        table = "inhibit"
        id = "inhibit"
        app = ""
        perms = ["logout", "suspend"]

        try:
            permission_store_intf.SetPermission(
                table,
                # Do not create if it does not exist
                False,
                id,
                app,
                perms,
            )
            assert False, "This statement should not be reached"
        except GLib.GError as e:
            assert "org.freedesktop.portal.Error.NotFound" in e.message

        permission_store_intf.SetPermission(table, True, id, app, perms)

    def test_delete(self, portals, dbus_con):
        permission_store_intf = PermissionStore()

        table = "inhibit"
        id = "inhibit"
        app = ""
        perms = ["logout", "suspend"]

        try:
            permission_store_intf.Delete(table, id)
            assert False, "This statement should not be reached"
        except GLib.GError as e:
            assert "org.freedesktop.portal.Error.NotFound" in e.message

        permission_store_intf.SetPermission(table, True, id, app, perms)

        permission_store_intf.Delete(table, id)

        try:
            permission_store_intf.Lookup(table, id)
            assert False, "This statement should not be reached"
        except GLib.GError as e:
            assert "org.freedesktop.portal.Error.NotFound" in e.message

    def test_get_permission(self, portals, dbus_con):
        permission_store_intf = PermissionStore()

        table = "notifications"
        id = "notification"
        app = "a"
        perms = ["yes"]

        try:
            permission_store_intf.GetPermission(table, id, app)
            assert False, "This statement should not be reached"
        except GLib.GError as e:
            assert "org.freedesktop.portal.Error.NotFound" in e.message

        permission_store_intf.SetPermission(table, True, id, app, perms)

        result, _ = permission_store_intf.GetPermission(table, id, app)
        permissions = result.unpack()[0]
        assert permissions == perms

        result, _ = permission_store_intf.GetPermission(table, id, "no-such-app")
        permissions = result.unpack()[0]
        assert permissions == []
