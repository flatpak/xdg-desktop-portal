# SPDX-License-Identifier: LGPL-2.1-or-later
# mypy: disable-error-code="misc"

from tests.templates.xdp_utils import init_logger

import dbus.service
import dbus

from dbusmock import mockobject

BUS_NAME = "org.freedesktop.GeoClue2"
MAIN_OBJ = "/org/freedesktop/GeoClue2/Manager"
MAIN_IFACE = "org.freedesktop.GeoClue2.Manager"
CLIENT_IFACE = "org.freedesktop.GeoClue2.Client"
LOCATION_IFACE = "org.freedesktop.GeoClue2.Location"
MOCK_IFACE = "org.freedesktop.GeoClue2.Mock"
SYSTEM_BUS = True
VERSION = 1

logger = init_logger(__name__)


class GeoClueClient(mockobject.DBusMockObject):
    def __init__(self, *args, **kwargs):
        super(GeoClueClient, self).__init__(*args, **kwargs)

        self.started = False
        self.location = 0
        self.data = {"Latitude": 0, "Longitude": 0, "Accuracy": 0}

    @dbus.service.method(
        CLIENT_IFACE,
        in_signature="",
        out_signature="",
    )
    def Start(self):
        logger.debug("Start()")
        self.started = True
        self.ChangeLocation(self.data)

    @dbus.service.method(
        CLIENT_IFACE,
        in_signature="",
        out_signature="",
    )
    def Stop(self):
        logger.debug("Stop()")
        self.started = False
        self.RemoveObject(f"/org/freedesktop/GeoClue2/Location/{self.location}")

    @dbus.service.method(
        MOCK_IFACE,
        in_signature="a{sv}",
        out_signature="",
    )
    def ChangeLocation(self, data):
        logger.debug(f"ChangeLocation({data})")

        self.data = data

        if not self.started:
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
                "Latitude": data["Latitude"],
                "Longitude": data["Longitude"],
                "Accuracy": data["Accuracy"],
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


def load(mock, parameters={}):
    mock.AddMethods(
        MAIN_IFACE,
        [
            (
                "GetClient",
                "",
                "o",
                'ret = dbus.ObjectPath("/org/freedesktop/GeoClue2/Client/1")',
            ),
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
        [],
        mock_class=GeoClueClient,
    )
