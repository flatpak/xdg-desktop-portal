# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors
#
# This file is formatted with Python Black
# mypy: disable-error-code="misc"

import tests.xdp_utils as xdp

import pytest
import dbus
import os


@pytest.fixture
def required_templates():
    return {
        "speechprovider": {},
        "access": {},
    }


class SpeechPermissions:
    def __init__(self, dbus_con, app_id):
        self.app_id = app_id
        self.permissions_store = xdp.get_permission_store_iface(dbus_con)

    def set_allowed_providers(self, allowed_providers):
        self.permissions_store.SetValue(
            "speech",
            True,
            "speech",
            dbus.Dictionary([(p, True) for p in allowed_providers], signature="sb"),
        )

    def set_permission(self, perm):
        self.permissions_store.SetPermission(
            "speech",
            True,
            "speech",
            self.app_id,
            [perm],
        )


@pytest.fixture
def speech_permissions(xdg_permission_store, xdp_app_info, dbus_con):
    speech_permissions = SpeechPermissions(dbus_con, xdp_app_info.app_id)
    speech_permissions.set_allowed_providers(["org.one.Speech.Provider"])
    speech_permissions.set_permission("yes")
    return speech_permissions


class SpeechPortalSignalListener:
    def __init__(self, dbus_con):
        self.recieved_portal_signals: dict[str, list[tuple]] = {}

        dbus_con.add_signal_receiver(
            self._signal_reciever,
            dbus_interface=xdp.portal_interface_name("Speech"),
            member_keyword="member",
        )

    def _signal_reciever(self, *args, **kwargs):
        member = kwargs["member"]
        self.recieved_portal_signals[member] = self.recieved_portal_signals.get(
            member, []
        ) + [args]

    def wait_for_portal_signal(self, signal_name, invoker):
        recieved_count = len(self.recieved_portal_signals.get(signal_name, []))

        invoker()

        xdp.wait_for(
            lambda: len(self.recieved_portal_signals.get(signal_name, []))
            > recieved_count
        )
        return self.recieved_portal_signals[signal_name].pop()


@pytest.fixture
def dbus_signal_reciever(dbus_con):
    dbus_signal_reciever = SpeechPortalSignalListener(dbus_con)
    return dbus_signal_reciever


@pytest.fixture
def speech_provider_mock(dbus_con):
    speechprovider_proxy = dbus_con.get_object(
        "org.one.Speech.Provider",
        "/org/one/Speech/Provider",
    )
    return dbus.Interface(speechprovider_proxy, "org.freedesktop.Speech.Provider.Mock")


@pytest.fixture
def speech_portal(dbus_con):
    return xdp.get_portal_iface(dbus_con, "Speech")


class TestSpeech:
    def get_provider_details(
        self, dbus_con, speech_portal, session, provider_id, expected_response=0
    ):
        get_provider_details_request = xdp.Request(dbus_con, speech_portal)
        get_provider_details_response = get_provider_details_request.call(
            "GetProviderDetails",
            session_handle=session.handle,
            parent_window="window-hndl",
            provider_id=provider_id,
            options={},
        )

        assert get_provider_details_response
        assert get_provider_details_response.response == expected_response
        return get_provider_details_response.results

    def get_voices(self, dbus_con, speech_portal, session, expected_response=0):
        get_voices_request = xdp.Request(dbus_con, speech_portal)
        get_voices_response = get_voices_request.call(
            "GetVoices",
            session_handle=session.handle,
            parent_window="window-hndl",
            options={},
        )

        assert get_voices_response
        assert get_voices_response.response == expected_response
        return get_voices_response.results.get("voices", [])

    def synthesize(
        self,
        dbus_con,
        speech_portal,
        session,
        provider_id,
        voice_id,
        expected_response=0,
    ):
        readfd, writefd = os.pipe()
        synthesize_request = xdp.Request(dbus_con, speech_portal)
        synthesize_response = synthesize_request.call(
            "Synthesize",
            session_handle=session.handle,
            parent_window="window-hndl",
            provider_id=provider_id,
            pipe_fd=dbus.types.UnixFd(writefd),
            text="HELLO world",
            voice_id=voice_id,
            pitch=10,
            rate=10,
            is_ssml=False,
            language="en",
            options={},
        )

        assert synthesize_response
        assert synthesize_response.response == expected_response

        if expected_response != 0:
            return

        f = os.fdopen(readfd, "r")

        # XXX: Write stream should be closed so we should be able to read to EOF but that is not the case.
        assert f.read(11) == "hello WORLD"
        f.close()

    def test_version(self, portals, dbus_con):
        xdp.check_version(dbus_con, "Speech", 1)

    def test_session_get_voices(
        self,
        portals,
        dbus_con,
        speech_permissions,
        dbus_signal_reciever,
        speech_provider_mock,
        speech_portal,
    ):
        session = xdp.Session(
            dbus_con,
            speech_portal.CreateSession({"session_handle_token": "session_token0"}),
        )

        voices = self.get_voices(dbus_con, speech_portal, session)
        assert len(voices) == 1

        assert voices[0]["name"] == "Armenian (West Armenia)"
        assert voices[0]["identifier"] == "ine/hyw"
        assert (
            voices[0]["output-format"]
            == "audio/x-raw,format=S16LE,channels=1,rate=22050"
        )
        assert voices[0]["features"] == 3
        assert voices[0]["languages"] == ["hyw", "hy-arevmda", "hy"]
        assert voices[0]["provider"] == "org.one.Speech.Provider"

        provider_details = self.get_provider_details(
            dbus_con, speech_portal, session, "org.one.Speech.Provider"
        )
        assert provider_details["name"] == "Mock Speech Provider"

        dbus_signal_reciever.wait_for_portal_signal(
            "VoicesChanged",
            lambda: speech_provider_mock.AddVoice(
                "English",
                "gmw/en-US",
                "audio/x-spiel,format=S16LE,channels=1,rate=22050",
                0,
                ["en-us", "en"],
            ),
        )

        voices = self.get_voices(dbus_con, speech_portal, session)
        assert len(voices) == 2

        dbus_signal_reciever.wait_for_portal_signal(
            "VoicesChanged", lambda: speech_provider_mock.Hide()
        )

        voices = self.get_voices(dbus_con, speech_portal, session)
        assert len(voices) == 0

        dbus_signal_reciever.wait_for_portal_signal(
            "VoicesChanged", lambda: speech_provider_mock.Show()
        )

        voices = self.get_voices(dbus_con, speech_portal, session)
        assert len(voices) == 2

        dbus_signal_reciever.wait_for_portal_signal(
            "VoicesChanged",
            lambda: speech_provider_mock.RemoveVoice(0),
        )

        voices = self.get_voices(dbus_con, speech_portal, session)
        assert len(voices) == 1

        # Close session
        session.close()

        try:
            self.get_voices(dbus_con, speech_portal, session)
        except dbus.exceptions.DBusException as exc:
            assert exc.get_dbus_name() == "org.freedesktop.DBus.Error.AccessDenied"
        else:
            raise AssertionError("No assertion was raised")

    def test_session_synthesize(
        self, portals, dbus_con, speech_permissions, speech_portal
    ):
        session = xdp.Session(
            dbus_con,
            speech_portal.CreateSession({"session_handle_token": "session_token0"}),
        )

        self.synthesize(
            dbus_con, speech_portal, session, "org.one.Speech.Provider", "gmw/en-US"
        )

        # Test synthesizing using unknown provider
        self.synthesize(
            dbus_con, speech_portal, session, "foo", "gmw/en-US", expected_response=2
        )

    def test_access_denied(
        self, portals, dbus_con, speech_permissions, dbus_signal_reciever, speech_portal
    ):
        speech_permissions.set_permission("no")

        session = xdp.Session(
            dbus_con,
            speech_portal.CreateSession({"session_handle_token": "session_token0"}),
        )

        self.get_voices(dbus_con, speech_portal, session, expected_response=1)

        self.get_provider_details(
            dbus_con,
            speech_portal,
            session,
            "org.one.Speech.Provider",
            expected_response=1,
        )

        self.synthesize(
            dbus_con,
            speech_portal,
            session,
            "org.one.Speech.Provider",
            "gmw/en-US",
            expected_response=1,
        )

        # Allowing the app will cause a providers changed signal
        dbus_signal_reciever.wait_for_portal_signal(
            "VoicesChanged", lambda: speech_permissions.set_permission("yes")
        )

        voices = self.get_voices(
            dbus_con,
            speech_portal,
            session,
        )
        assert len(voices) == 1

    def test_providers_allow_list(
        self, portals, dbus_con, speech_permissions, dbus_signal_reciever, speech_portal
    ):
        session = xdp.Session(
            dbus_con,
            speech_portal.CreateSession({"session_handle_token": "session_token0"}),
        )

        self.get_provider_details(
            dbus_con, speech_portal, session, "org.one.Speech.Provider"
        )

        voices = self.get_voices(dbus_con, speech_portal, session)
        assert len(voices) == 1

        dbus_signal_reciever.wait_for_portal_signal(
            "VoicesChanged",
            lambda: speech_permissions.set_allowed_providers(
                ["org.two.Speech.Provider"]
            ),
        )

        # Expect a response of OTHER because access was not denied to the app but the provider is hidden from it
        self.get_provider_details(
            dbus_con,
            speech_portal,
            session,
            "org.one.Speech.Provider",
            expected_response=2,
        )

        voices = self.get_voices(dbus_con, speech_portal, session)
        assert len(voices) == 0

        # Expect a response of OTHER because access was not denied to the app but the provider is hidden from it
        self.synthesize(
            dbus_con,
            speech_portal,
            session,
            "org.one.Speech.Provider",
            "gmw/en-US",
            expected_response=2,
        )

        dbus_signal_reciever.wait_for_portal_signal(
            "VoicesChanged",
            lambda: speech_permissions.set_allowed_providers(
                ["org.one.Speech.Provider"]
            ),
        )

        self.get_provider_details(
            dbus_con, speech_portal, session, "org.one.Speech.Provider"
        )

        voices = self.get_voices(dbus_con, speech_portal, session)
        assert len(voices) == 1
