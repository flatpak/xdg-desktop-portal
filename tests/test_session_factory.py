# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

from tests import Request, PortalTest
from gi.repository import GLib

class TestSessionFactory(PortalTest):
    def test_version(self):
        self.check_version(1)

    def test_create_session(self):
        self.start_xdp()

        session_intf = self.get_dbus_interface()
        request = Request(self.dbus_con, session_intf)
        options = {
            "session_handle_token": "token0",
        }
        response = request.call(
            "CreateSession",
            options=options,
        )
        assert response.response == 0
        results = response.results
        assert "session_handle" in results
        assert results["session_handle"].endswith("token0")
