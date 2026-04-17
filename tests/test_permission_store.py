# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests.xdp_utils as xdp

import dbus
import pytest
from enum import Enum
from gi.repository import GLib, Gio


class PermissionsIDType(Enum):
    PERMISSIONS_ID = 1
    APP_ID = 2


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

    @pytest.mark.parametrize("permission_id_type", [*PermissionsIDType])
    def test_delete_race(self, portals, dbus_con, xdp_app_info, permission_id_type):
        permission_store_intf = PermissionStore()
        finished_count = 0

        table = "inhibit"
        id = "inhibit"
        perms = ["logout", "suspend"]

        permissions_id = xdp_app_info.permissions_id
        other_permissions_id = xdp_app_info.app_id
        if permission_id_type == PermissionsIDType.APP_ID:
            permissions_id = xdp_app_info.app_id
            other_permissions_id = xdp_app_info.permissions_id

        def cb(_):
            nonlocal finished_count

            finished_count += 1

        permission_store_intf.SetPermissionAsync(table, True, id, "a", perms, cb)
        permission_store_intf.SetPermissionAsync(
            table, True, id, permissions_id, perms, cb
        )
        permission_store_intf.DeleteAsync(table, id, cb)

        xdp.wait_for(lambda: finished_count >= 3)
        finished_count = 0

        try:
            permission_store_intf.Lookup(table, id)
            assert False, "This statement should not be reached"
        except GLib.GError as e:
            assert "org.freedesktop.portal.Error.NotFound" in e.message
            assert e.matches(Gio.io_error_quark(), Gio.IOErrorEnum.DBUS_ERROR)

        permission_store_intf.SetPermissionAsync(
            table, True, id, permissions_id, perms, cb
        )
        permission_store_intf.SetPermissionAsync(
            table, True, id, other_permissions_id, perms, cb
        )
        permission_store_intf.SetPermissionAsync(table, True, id, "a", perms, cb)
        permission_store_intf.SetPermissionAsync(table, True, id, "b", perms, cb)
        permission_store_intf.DeletePermissionAsync(table, id, "a", cb)

        xdp.wait_for(lambda: finished_count >= 5)
        finished_count = 0

        result, _ = permission_store_intf.Lookup(table, id)
        perms_out = result.unpack()[0]
        assert perms_out == {"b": perms, xdp_app_info.permissions_id: perms}

        permission_store_intf.SetPermissionAsync(table, True, id, "a", perms, cb)
        permission_store_intf.DeletePermissionAsync(table, id, "b", cb)
        permission_store_intf.DeletePermissionAsync(table, id, "a", cb)
        permission_store_intf.DeletePermissionAsync(table, id, permissions_id, cb)

        xdp.wait_for(lambda: finished_count >= 4)
        finished_count = 0

        result, _ = permission_store_intf.Lookup(table, id)
        perms_out = result.unpack()[0]
        assert perms_out == {}

        permission_store_intf.SetPermissionAsync(
            table, True, id, permissions_id, perms, cb
        )
        permission_store_intf.DeletePermissionAsync(table, id, other_permissions_id, cb)

        xdp.wait_for(lambda: finished_count >= 2)
        finished_count = 0

        result, _ = permission_store_intf.Lookup(table, id)
        perms_out = result.unpack()[0]
        assert perms_out == {}

    @pytest.mark.parametrize("permission_id_type", [*PermissionsIDType])
    def test_change(self, portals, dbus_con, xdp_app_info, permission_id_type):
        permission_store_intf = PermissionStore()
        changed_count = 0

        table = "TEST"
        id = "test-resource"
        perms = ["one", "two"]
        permissions_id = xdp_app_info.permissions_id

        if permission_id_type == PermissionsIDType.APP_ID:
            permissions_id = xdp_app_info.app_id

        def cb_changed1(results):
            nonlocal changed_count

            cb_table, cb_id, deleted, _, cb_perms = results.unpack()

            assert cb_table == table
            assert cb_id == id
            assert not deleted
            assert cb_perms[xdp_app_info.permissions_id] == perms

            changed_count += 1

        cs = permission_store_intf.connect_to_signal("Changed", cb_changed1)
        permission_store_intf.SetPermissionAsync(
            table, True, id, permissions_id, perms, None
        )
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

    @pytest.mark.parametrize("permission_id_type", [*PermissionsIDType])
    def test_create(self, portals, dbus_con, xdp_app_info, permission_id_type):
        permission_store_intf = PermissionStore()

        table = "inhibit"
        id = "inhibit"
        permissions_id = xdp_app_info.permissions_id
        perms = ["logout", "suspend"]

        if permission_id_type == PermissionsIDType.APP_ID:
            permissions_id = xdp_app_info.app_id

        try:
            permission_store_intf.SetPermission(
                table,
                # Do not create if it does not exist
                False,
                id,
                permissions_id,
                perms,
            )
            assert False, "This statement should not be reached"
        except GLib.GError as e:
            assert "org.freedesktop.portal.Error.NotFound" in e.message

        permission_store_intf.SetPermission(table, True, id, permissions_id, perms)

    @pytest.mark.parametrize("permission_id_type", [*PermissionsIDType])
    def test_delete(self, portals, dbus_con, xdp_app_info, permission_id_type):
        permission_store_intf = PermissionStore()

        table = "inhibit"
        id = "inhibit"
        permissions_id = xdp_app_info.permissions_id
        perms = ["logout", "suspend"]

        if permission_id_type == PermissionsIDType.APP_ID:
            permissions_id = xdp_app_info.app_id

        try:
            permission_store_intf.Delete(table, id)
            assert False, "This statement should not be reached"
        except GLib.GError as e:
            assert "org.freedesktop.portal.Error.NotFound" in e.message

        permission_store_intf.SetPermission(table, True, id, permissions_id, perms)

        permission_store_intf.Delete(table, id)

        try:
            permission_store_intf.Lookup(table, id)
            assert False, "This statement should not be reached"
        except GLib.GError as e:
            assert "org.freedesktop.portal.Error.NotFound" in e.message

    @pytest.mark.parametrize("permission_id_type", [*PermissionsIDType])
    def test_get_permission(self, portals, dbus_con, xdp_app_info, permission_id_type):
        permission_store_intf = PermissionStore()

        table = "notifications"
        id = "notification"
        permissions_id = xdp_app_info.permissions_id
        other_permissions_id = xdp_app_info.app_id
        perms = ["yes"]

        if permission_id_type == PermissionsIDType.APP_ID:
            permissions_id = xdp_app_info.app_id
            other_permissions_id = xdp_app_info.permissions_id

        try:
            permission_store_intf.GetPermission(table, id, permissions_id)
            assert False, "This statement should not be reached"
        except GLib.GError as e:
            assert "org.freedesktop.portal.Error.NotFound" in e.message

        permission_store_intf.SetPermission(table, True, id, permissions_id, perms)

        result, _ = permission_store_intf.GetPermission(table, id, permissions_id)
        permissions = result.unpack()[0]
        assert permissions == perms

        result, _ = permission_store_intf.GetPermission(table, id, other_permissions_id)
        permissions = result.unpack()[0]
        assert permissions == perms

        result, _ = permission_store_intf.GetPermission(table, id, "no-such-app")
        permissions = result.unpack()[0]
        assert permissions == []
