# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black

import tests as xdp

import dbus
import pytest
import time


@pytest.fixture
def required_templates():
    return {"email": {}}


class TestEmail:
    def test_version(self, portals, dbus_con):
        """tests the version of the interface"""

        xdp.check_version(dbus_con, "Email", 4)

    def test_email_basic(self, portals, dbus_con):
        """test that the backend receives the expected data"""

        email_intf = xdp.get_portal_iface(dbus_con, "Email")
        mock_intf = xdp.get_mock_iface(dbus_con)

        addresses = ["mclasen@redhat.com"]
        subject = "Re: portal tests"
        body = "You have to see this"

        request = xdp.Request(dbus_con, email_intf)
        options = {
            "addresses": addresses,
            "subject": subject,
            "body": body,
        }
        response = request.call(
            "ComposeEmail",
            parent_window="",
            options=options,
        )

        assert response
        assert response.response == 0

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("ComposeEmail")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[2] == ""  # parent window
        assert args[3]["addresses"] == addresses
        assert args[3]["subject"] == subject
        assert args[3]["body"] == body

    def test_email_address(self, portals, dbus_con):
        """test that an invalid address triggers an error"""

        email_intf = xdp.get_portal_iface(dbus_con, "Email")
        mock_intf = xdp.get_mock_iface(dbus_con)

        addresses = ["gibberish! not an email address\n%Q"]

        request = xdp.Request(dbus_con, email_intf)
        options = {
            "addresses": addresses,
        }
        with pytest.raises(dbus.exceptions.DBusException) as excinfo:
            request.call(
                "ComposeEmail",
                parent_window="",
                options=options,
            )
        assert (
            excinfo.value.get_dbus_name()
            == "org.freedesktop.portal.Error.InvalidArgument"
        )

        # Check the impl portal was never called
        method_calls = mock_intf.GetMethodCalls("ComposeEmail")
        assert len(method_calls) == 0

    def test_email_punycode_address(self, portals, dbus_con):
        """test email address containing punycode"""

        email_intf = xdp.get_portal_iface(dbus_con, "Email")
        mock_intf = xdp.get_mock_iface(dbus_con)

        addresses = ["xn--franais-xxa@exemple.fr"]
        subject = "Re: portal tests"
        body = "To ASCII and beyond"

        request = xdp.Request(dbus_con, email_intf)
        options = {
            "addresses": addresses,
            "subject": subject,
            "body": body,
        }
        response = request.call(
            "ComposeEmail",
            parent_window="",
            options=options,
        )

        assert response
        assert response.response == 0

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("ComposeEmail")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[2] == ""  # parent window
        assert args[3]["addresses"] == addresses
        assert args[3]["subject"] == subject
        assert args[3]["body"] == body

    def test_email_subject_multiline(self, portals, dbus_con):
        """test that an multiline subject triggers an error"""

        email_intf = xdp.get_portal_iface(dbus_con, "Email")
        mock_intf = xdp.get_mock_iface(dbus_con)

        subject = "not\na\nvalid\nsubject line"

        request = xdp.Request(dbus_con, email_intf)
        options = {
            "subject": subject,
        }
        with pytest.raises(dbus.exceptions.DBusException) as excinfo:
            request.call(
                "ComposeEmail",
                parent_window="",
                options=options,
            )
        assert (
            excinfo.value.get_dbus_name()
            == "org.freedesktop.portal.Error.InvalidArgument"
        )

        # Check the impl portal was never called
        method_calls = mock_intf.GetMethodCalls("ComposeEmail")
        assert len(method_calls) == 0

    def test_email_subject_too_long(self, portals, dbus_con):
        """test that a subject line over 200 chars triggers an error"""

        email_intf = xdp.get_portal_iface(dbus_con, "Email")
        mock_intf = xdp.get_mock_iface(dbus_con)

        subject = "This subject line is too long" + "abc" * 60

        assert len(subject) > 200

        request = xdp.Request(dbus_con, email_intf)
        options = {
            "subject": subject,
        }
        with pytest.raises(dbus.exceptions.DBusException) as excinfo:
            request.call(
                "ComposeEmail",
                parent_window="",
                options=options,
            )
        assert (
            excinfo.value.get_dbus_name()
            == "org.freedesktop.portal.Error.InvalidArgument"
        )

        # Check the impl portal was never called
        method_calls = mock_intf.GetMethodCalls("ComposeEmail")
        assert len(method_calls) == 0

    @pytest.mark.parametrize("template_params", ({"email": {"delay": 2000}},))
    def test_email_delay(self, portals, dbus_con):
        """
        Test that everything works as expected when the backend takes some
        time to send its response, as * is to be expected from a real backend
        that presents dialogs to the user.
        """

        email_intf = xdp.get_portal_iface(dbus_con, "Email")
        mock_intf = xdp.get_mock_iface(dbus_con)

        subject = "delay test"
        addresses = ["mclasen@redhat.com"]

        request = xdp.Request(dbus_con, email_intf)
        options = {
            "addresses": addresses,
            "subject": subject,
        }

        start_time = time.perf_counter()

        response = request.call(
            "ComposeEmail",
            parent_window="",
            options=options,
        )

        assert response
        assert response.response == 0

        end_time = time.perf_counter()

        assert end_time - start_time > 2

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("ComposeEmail")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[2] == ""  # parent window
        assert args[3]["addresses"] == addresses
        assert args[3]["subject"] == subject

    @pytest.mark.parametrize("template_params", ({"email": {"response": 1}},))
    def test_email_cancel(self, portals, dbus_con):
        """
        Test that user cancellation works as expected.
        We simulate that the user cancels a hypothetical dialog,
        by telling the backend to return 1 as response code.
        And we check that we get the expected G_IO_ERROR_CANCELLED.
        """

        email_intf = xdp.get_portal_iface(dbus_con, "Email")
        mock_intf = xdp.get_mock_iface(dbus_con)

        subject = "cancel test"
        addresses = ["mclasen@redhat.com"]

        request = xdp.Request(dbus_con, email_intf)
        options = {
            "addresses": addresses,
            "subject": subject,
        }

        response = request.call(
            "ComposeEmail",
            parent_window="",
            options=options,
        )

        assert response
        assert response.response == 1

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("ComposeEmail")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[2] == ""  # parent window
        assert args[3]["addresses"] == addresses
        assert args[3]["subject"] == subject

    @pytest.mark.parametrize("template_params", ({"email": {"expect-close": True}},))
    def test_email_close(self, portals, dbus_con):
        """
        Test that app-side cancellation works as expected.
        We cancel the cancellable while while the hypothetical
        dialog is up, and tell the backend that it should
        expect a Close call. We rely on the backend to
        verify that that call actually happened.
        """

        email_intf = xdp.get_portal_iface(dbus_con, "Email")
        mock_intf = xdp.get_mock_iface(dbus_con)

        subject = "close test"
        addresses = ["mclasen@redhat.com"]

        request = xdp.Request(dbus_con, email_intf)
        request.schedule_close(1000)
        options = {
            "addresses": addresses,
            "subject": subject,
        }

        request.call(
            "ComposeEmail",
            parent_window="",
            options=options,
        )

        # Only true if the impl.Request was closed too
        assert request.closed

        # Check the impl portal was called with the right args
        method_calls = mock_intf.GetMethodCalls("ComposeEmail")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[2] == ""  # parent window
        assert args[3]["addresses"] == addresses
        assert args[3]["subject"] == subject
