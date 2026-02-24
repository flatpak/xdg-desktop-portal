# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

from tests.templates.xdp_utils import init_logger
import dbus.service
import dbus
from gi.repository import GLib
import os

SYSTEM_BUS = False
MAIN_IFACE = "org.freedesktop.Speech.Provider"
MOCK_IFACE = "org.freedesktop.Speech.Provider.Mock"
BUS_NAME = "org.one.Speech.Provider"
MAIN_OBJ = "/org/one/Speech/Provider"

VERSION = 1

logger = init_logger(__name__)

PROPS = {
    "Name": "Mock Speech Provider",
    "Voices": dbus.Array(
        [
            (
                "Armenian (West Armenia)",
                "audio/x-raw,format=S16LE,channels=1,rate=22050",
                "ine/hyw",
                0,
                ["hyw", "hy-arevmda", "hy"],
            )
        ],
        signature=dbus.Signature("(ssstas)"),
    ),
}


def load(mock, parameters={}):
    mock.AddProperties(MAIN_IFACE, PROPS)
    logger.debug(f"Loading parameters: {parameters}")


@dbus.service.method(
    MAIN_IFACE,
    in_signature="hssddbs",
    out_signature="",
)
def Synthesize(self, pipe_fd, text, voice_id, pitch, rate, is_ssml, language):
    fd = pipe_fd.take()
    f = os.fdopen(fd, "w")
    f.write(text.swapcase())
    f.close()


@dbus.service.method(
    MOCK_IFACE,
    in_signature="",
    out_signature="",
)
def Hide(self):
    name = self.bus_name.get_name()
    if not name.endswith("_"):
        self.bus_name.get_bus().release_name(name)
        self.bus_name._name = f"{name}_"
        self.bus_name.get_bus().request_name(self.bus_name._name, 0)


@dbus.service.method(
    MOCK_IFACE,
    in_signature="",
    out_signature="",
)
def Show(self):
    name = self.bus_name.get_name()
    if name.endswith("_"):
        self.bus_name.get_bus().release_name(name)
        self.bus_name._name = name[:-1]
        self.bus_name.get_bus().request_name(self.bus_name._name, 0)


def emit_voices_changes(mock_obj):
    mock_obj.UpdateProperties(MAIN_IFACE, {"Voices": PROPS["Voices"]})


@dbus.service.method(
    MOCK_IFACE,
    in_signature="ssstas",
    out_signature="t",
)
def AddVoice(self, name, identifier, output_format, features, languages):
    PROPS["Voices"].append((name, identifier, output_format, features, languages))
    GLib.idle_add(emit_voices_changes, self)
    return len(PROPS["Voices"])


@dbus.service.method(
    MOCK_IFACE,
    in_signature="t",
    out_signature="t",
)
def RemoveVoice(self, index):
    PROPS["Voices"].pop(index)
    GLib.idle_add(emit_voices_changes, self)
    return len(PROPS["Voices"])
