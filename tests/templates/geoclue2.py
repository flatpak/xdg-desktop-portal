# SPDX-License-Identifier: LGPL-2.1-or-later

from tests.templates import init_template_logger
import dbus.service
import dbus
import tempfile

from dbusmock import mockobject

from gi.repository import GLib

BUS_NAME = "org.freedesktop.GeoClue2"
MAIN_OBJ = "/org/freedesktop/GeoClue2/Manager"
MAIN_IFACE = "org.freedesktop.GeoClue2.Manager"
CLIENT_IFACE = "org.freedesktop.GeoClue2.Client"
LOCATION_IFACE = "org.freedesktop.GeoClue2.Location"
MOCK_IFACE = "org.freedesktop.GeoClue2.Mock"
SYSTEM_BUS = True
VERSION = 1

logger = init_template_logger(__name__)


def load(mock, parameters={}):
    mock.AddMethods(
        MAIN_IFACE,
        [
            ("GetClient", "", "o", 'ret = dbus.ObjectPath("/org/freedesktop/GeoClue2/Client/1")'),
        ],
    )
    mock.AddObject(
        "/org/freedesktop/GeoClue2/Client/1",
        CLIENT_IFACE,
        {
            "DesktopId": "",
            "DistanceThreshold": 0,
            "TimeThreshold": 0,
            "RequestedAccuracyLevel": 0,
        },
        [
            ("Start", "", "", Start),
            ("Stop", "", "", Stop),
        ],
    )
    mock.client = mockobject.objects["/org/freedesktop/GeoClue2/Client/1"]
    mock.client.manager = mock
    mock.client.started = False
    mock.client.location = 0
    mock.client.props = {
        "Latitude": 0,
        "Longitude": 0,
        "Accuracy": 0
    }

    mock.client.AddMethod(MOCK_IFACE, "ChangeLocation", "a{sv}", "", ChangeLocation)


@dbus.service.method(
    CLIENT_IFACE,
    in_signature="",
    out_signature="",
)
def Start(self):
    logger.debug(f"Start()")
    self.started = True
    self.ChangeLocation(self.props)


@dbus.service.method(
    CLIENT_IFACE,
    in_signature="",
    out_signature="",
)
def Stop(self):
    logger.debug(f"Stop()")
    self.started = False
    self.RemoveObject(f"/org/freedesktop/GeoClue2/Location/{self.location}")


@dbus.service.method(
    MOCK_IFACE,
    in_signature="a{sv}",
    out_signature="",
)
def ChangeLocation(self, props):
    logger.debug(f"ChangeLocation({props})")

    self.props = props

    if self.started != True:
        return

    old_path = "/"
    if self.location > 0:
        old_path = f"/org/freedesktop/GeoClue2/Location/{self.location}"
    self.location = self.location + 1
    new_path = f"/org/freedesktop/GeoClue2/Location/{self.location}"

    self.AddObject(
        new_path,
        LOCATION_IFACE,
        {
            "Latitude": props["Latitude"],
            "Longitude": props["Longitude"],
            "Accuracy": props["Accuracy"],
        },
        [],
    )

    if old_path != "/":
        self.RemoveObject(old_path)

    self.EmitSignal(
        CLIENT_IFACE,
        "LocationUpdated",
        "oo",
        [
            dbus.ObjectPath(old_path),
            dbus.ObjectPath(new_path),
        ],
    )

