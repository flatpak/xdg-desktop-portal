# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black

import signal
import subprocess
from typing import Any, AnyStr

import dbus
import pytest

import tests.xdp_utils as xdp


@pytest.fixture
def xdp_app_info() -> xdp.AppInfoHost:
    return xdp.AppInfoHost()


@pytest.fixture
def required_templates() -> xdp.RequiredTemplates:
    return {"email": {"delay": 30000}}


@pytest.fixture
def xdg_desktop_portal_options() -> xdp.PortalProcessOptions:
    return xdp.PortalProcessOptions(capture_stderr=True)


class TestShutdown:
    def test_shutdown_with_inflight_call(
        self,
        portals: Any,
        dbus_con: dbus.Bus,
        xdg_desktop_portal: subprocess.Popen[AnyStr],
    ) -> None:
        """
        Test that the portal shuts down cleanly with an in-flight backend call.

        Send a ComposeEmail call with a very long backend delay so the call
        is still pending when SIGHUP arrives. Verify the portal exits cleanly.
        """
        email_intf = xdp.get_portal_iface(dbus_con, "Email")
        mock_intf = xdp.get_mock_iface(dbus_con)

        email_intf.ComposeEmail(
            "",
            dbus.Dictionary(
                {
                    "addresses": dbus.Array(["test@example.com"], signature="s"),
                    "subject": "shutdown test",
                    "handle_token": dbus.String("request_shutdown", variant_level=1),
                },
                signature="sv",
            ),
            reply_handler=lambda *args: None,
            error_handler=lambda *args: None,
        )

        # Wait for the call to reach the backend, confirming it is
        # in-flight before we send SIGHUP.
        xdp.wait_for(lambda: len(mock_intf.GetMethodCalls("ComposeEmail")) > 0)

        xdg_desktop_portal.send_signal(signal.SIGHUP)
        xdg_desktop_portal.communicate(timeout=10)

        assert xdg_desktop_portal.returncode == 0, "Portal did not exit cleanly"

        # TODO: once portals use fiber dispatch, verify that cancelled fibers
        # actually unwind through the error path:
        # stderr_text = stderr.decode("utf-8", errors="replace")
        # assert "Backend call failed: Fiber cancelled" in stderr_text
