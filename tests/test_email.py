# SPDX-License-Identifier: LGPL-2.1-or-later
#
# This file is formatted with Python Black


from tests import PortalTest

import dbus
import time


class TestEmail(PortalTest):
    def test_version(self):
        self.check_version(3)

    def test_email_basic(self):
        self.start_impl_portal()
        self.start_xdp()

        addresses = ["mclasen@redhat.com"]
        subject = "Re: portal tests"
        body = "You have to see this"

        request = self.create_request()
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

        assert response.response == 0

        # Check the impl portal was called with the right args
        method_calls = self.mock_interface.GetMethodCalls("ComposeEmail")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[2] == ""  # parent window
        assert args[3]["addresses"] == addresses
        assert args[3]["subject"] == subject
        assert args[3]["body"] == body

    def test_email_address(self):
        """test that an invalid address triggers an error"""

        self.start_impl_portal()
        self.start_xdp()

        addresses = ["gibberish! not an email address\n%Q"]

        request = self.create_request()
        options = {
            "addresses": addresses,
        }
        try:
            request.call(
                "ComposeEmail",
                parent_window="",
                options=options,
            )

            self.fail("This statement should not be reached")
        except dbus.exceptions.DBusException as e:
            assert e.get_dbus_name() == "org.freedesktop.portal.Error.InvalidArgument"

        # Check the impl portal was never called
        method_calls = self.mock_interface.GetMethodCalls("ComposeEmail")
        assert len(method_calls) == 0

    def test_email_subject_multiline(self):
        """test that an multiline subject triggers an error"""

        self.start_impl_portal()
        self.start_xdp()

        subject = "not\na\nvalid\nsubject line"

        request = self.create_request()
        options = {
            "subject": subject,
        }
        try:
            request.call(
                "ComposeEmail",
                parent_window="",
                options=options,
            )

            self.fail("This statement should not be reached")
        except dbus.exceptions.DBusException as e:
            assert e.get_dbus_name() == "org.freedesktop.portal.Error.InvalidArgument"

        # Check the impl portal was never called
        method_calls = self.mock_interface.GetMethodCalls("ComposeEmail")
        assert len(method_calls) == 0

    def test_email_subject_too_long(self):
        """test that a subject line over 200 chars triggers an error"""

        self.start_impl_portal()
        self.start_xdp()

        subject = "This subject line is too long" + "abc" * 60

        assert len(subject) > 200

        request = self.create_request()
        options = {
            "subject": subject,
        }
        try:
            request.call(
                "ComposeEmail",
                parent_window="",
                options=options,
            )

            self.fail("This statement should not be reached")
        except dbus.exceptions.DBusException as e:
            assert e.get_dbus_name() == "org.freedesktop.portal.Error.InvalidArgument"

        # Check the impl portal was never called
        method_calls = self.mock_interface.GetMethodCalls("ComposeEmail")
        assert len(method_calls) == 0

    def test_email_delay(self):
        """
        Test that everything works as expected when the backend takes some
        time to send its response, as * is to be expected from a real backend
        that presents dialogs to the user.
        """

        params = {"delay": 2000}

        self.start_impl_portal(params)
        self.start_xdp()

        subject = "delay test"
        addresses = ["mclasen@redhat.com"]

        request = self.create_request()
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

        assert response.response == 0

        end_time = time.perf_counter()

        assert end_time - start_time > 2

        # Check the impl portal was called with the right args
        method_calls = self.mock_interface.GetMethodCalls("ComposeEmail")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[2] == ""  # parent window
        assert args[3]["addresses"] == addresses
        assert args[3]["subject"] == subject

    def test_email_cancel(self):
        """
        Test that user cancellation works as expected.
        We simulate that the user cancels a hypothetical dialog,
        by telling the backend to return 1 as response code.
        And we check that we get the expected G_IO_ERROR_CANCELLED.
        """

        params = {"response": 1}

        self.start_impl_portal(params)
        self.start_xdp()

        subject = "cancel test"
        addresses = ["mclasen@redhat.com"]

        request = self.create_request()
        options = {
            "addresses": addresses,
            "subject": subject,
        }

        response = request.call(
            "ComposeEmail",
            parent_window="",
            options=options,
        )

        assert response.response == 1

        # Check the impl portal was called with the right args
        method_calls = self.mock_interface.GetMethodCalls("ComposeEmail")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[2] == ""  # parent window
        assert args[3]["addresses"] == addresses
        assert args[3]["subject"] == subject

    def test_email_close(self):
        """
        Test that app-side cancellation works as expected.
        We cancel the cancellable while while the hypothetical
        dialog is up, and tell the backend that it should
        expect a Close call. We rely on the backend to
        verify that that call actually happened.
        """

        params = {"expect-close": True}

        self.start_impl_portal(params)
        self.start_xdp()

        subject = "close test"
        addresses = ["mclasen@redhat.com"]

        request = self.create_request()
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
        method_calls = self.mock_interface.GetMethodCalls("ComposeEmail")
        assert len(method_calls) > 0
        _, args = method_calls[-1]
        assert args[2] == ""  # parent window
        assert args[3]["addresses"] == addresses
        assert args[3]["subject"] == subject
