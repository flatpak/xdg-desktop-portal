# SPDX-License-Identifier: LGPL-2.1-or-later

import tests.xdp_utils as xdp

import pytest
import dbus
import os


@pytest.fixture
def required_templates():
    return {"speechprovider": {}}


class TestSpeech:
    def set_permissions(self, dbus_con, appid, permissions):
        perm_store_intf = xdp.get_permission_store_iface(dbus_con)
        perm_store_intf.SetPermission(
            "speech",
            True,
            "speech",
            appid,
            permissions,
        )

    def get_speechprovider_mock(self, dbus_con):
        speechprovider_proxy = dbus_con.get_object(
            "org.one.Speech.Provider",
            "/org/one/Speech/Provider",
        )
        return dbus.Interface(
            speechprovider_proxy, "org.freedesktop.Speech.Provider.Mock"
        )

    def get_providers(self, dbus_con, synth_intf, session):
        get_providers_request = xdp.Request(dbus_con, synth_intf)
        get_providers_response = get_providers_request.call(
            "GetProviders",
            session_handle=session.handle,
            parent_window="window-hndl",
            options={},
        )

        assert get_providers_response
        assert get_providers_response.response == 0
        return get_providers_response.results["providers"]

    def get_voices(self, dbus_con, synth_intf, session, provider):
        get_voices_request = xdp.Request(dbus_con, synth_intf)
        get_voices_response = get_voices_request.call(
            "GetVoices",
            session_handle=session.handle,
            parent_window="window-hndl",
            provider_id=provider,
            options={},
        )

        assert get_voices_response
        assert get_voices_response.response == 0
        return get_voices_response.results["voices"]

    def listen_to_synth_signals(self, dbus_con):
        self.recieved_synth_signals: dict[str, list[tuple]] = {}

        def signal_reciever(*args, **kwargs):
            member = kwargs["member"]
            self.recieved_synth_signals[member] = self.recieved_synth_signals.get(
                "member", []
            ) + [args]

        dbus_con.add_signal_receiver(
            signal_reciever,
            dbus_interface=xdp.portal_interface_name("Speech"),
            member_keyword="member",
        )

    def wait_for_synth_signal(self, signal_name, invoker):
        recieved_count = len(self.recieved_synth_signals.get(signal_name, []))

        invoker()

        xdp.wait_for(
            lambda: len(self.recieved_synth_signals.get(signal_name, []))
            > recieved_count
        )
        return self.recieved_synth_signals[signal_name].pop()

    def test_version(self, portals, dbus_con):
        xdp.check_version(dbus_con, "Speech", 1)

    def test_session_get_providers(self, portals, dbus_con, dbus_con_sys):
        self.listen_to_synth_signals(dbus_con)
        mock = self.get_speechprovider_mock(dbus_con)
        synth_intf = xdp.get_portal_iface(dbus_con, "Speech")

        session = xdp.Session(
            dbus_con,
            synth_intf.CreateSession({"session_handle_token": "session_token0"}),
        )

        providers = self.get_providers(dbus_con, synth_intf, session)

        assert len(providers) == 1
        provider_id, provider_name = providers[0]
        assert provider_id == "org.one.Speech.Provider"
        assert provider_name == "Mock Speech Provider"

        voices = self.get_voices(dbus_con, synth_intf, session, provider_id)
        assert len(voices) == 1

        assert voices[0] == (
            "Armenian (West Armenia)",
            "audio/x-raw,format=S16LE,channels=1,rate=22050",
            "ine/hyw",
            0,
            ["hyw", "hy-arevmda", "hy"],
        )

        _, provider_id = self.wait_for_synth_signal(
            "VoicesChanged",
            lambda: mock.AddVoice(
                "English",
                "gmw/en-US",
                "audio/x-spiel,format=S16LE,channels=1,rate=22050",
                0,
                ["en-us", "en"],
            ),
        )

        voices = self.get_voices(dbus_con, synth_intf, session, provider_id)
        assert len(voices) == 2

        self.wait_for_synth_signal("ProvidersChanged", lambda: mock.Hide())

        providers = self.get_providers(dbus_con, synth_intf, session)
        assert len(providers) == 0

        get_voices_request = xdp.Request(dbus_con, synth_intf)
        get_voices_response = get_voices_request.call(
            "GetVoices",
            session_handle=session.handle,
            parent_window="window-hndl",
            provider_id=provider_id,
            options={},
        )

        assert get_voices_response
        # "Other" response because no provider was found.
        assert get_voices_response.response == 2

        self.wait_for_synth_signal("ProvidersChanged", lambda: mock.Show())

        providers = self.get_providers(dbus_con, synth_intf, session)
        assert len(providers) == 1

        voices = self.get_voices(dbus_con, synth_intf, session, provider_id)
        assert len(voices) == 2

        _, provider_id = self.wait_for_synth_signal(
            "VoicesChanged",
            lambda: mock.RemoveVoice(0),
        )

        voices = self.get_voices(dbus_con, synth_intf, session, provider_id)
        assert len(voices) == 1

        # Close session
        session.close()

        try:
            self.get_providers(dbus_con, synth_intf, session)
        except dbus.exceptions.DBusException as exc:
            assert exc.get_dbus_name() == "org.freedesktop.DBus.Error.AccessDenied"
        else:
            raise AssertionError("No assertion was raised")

    def test_session_synthesize(self, portals, dbus_con, dbus_con_sys):
        self.listen_to_synth_signals(dbus_con)
        synth_intf = xdp.get_portal_iface(dbus_con, "Speech")

        session = xdp.Session(
            dbus_con,
            synth_intf.CreateSession({"session_handle_token": "session_token0"}),
        )

        providers = self.get_providers(dbus_con, synth_intf, session)
        provider_id, provider_name = providers[0]

        readfd, writefd = os.pipe()
        synthesize_request = xdp.Request(dbus_con, synth_intf)
        synthesize_response = synthesize_request.call(
            "Synthesize",
            session_handle=session.handle,
            parent_window="window-hndl",
            provider_id=provider_id,
            pipe_fd=dbus.types.UnixFd(writefd),
            text="HELLO world",
            voice_id="voice-id",
            pitch=10,
            rate=10,
            is_ssml=False,
            language="en",
            options={},
        )

        assert synthesize_response
        assert synthesize_response.response == 0

        f = os.fdopen(readfd, "r")

        # XXX: Write stream should be closed so we should be able to read to EOF but that is not the case.
        assert f.read(11) == "hello WORLD"
        f.close()

        # Test synthesizing using unknown provider
        _, writefd = os.pipe()
        synthesize_request = xdp.Request(dbus_con, synth_intf)
        synthesize_response = synthesize_request.call(
            "Synthesize",
            session_handle=session.handle,
            parent_window="window-hndl",
            provider_id="foo",
            pipe_fd=dbus.types.UnixFd(writefd),
            text="HELLO world",
            voice_id="voice-id",
            pitch=10,
            rate=10,
            is_ssml=False,
            language="en",
            options={},
        )

        assert synthesize_response
        assert synthesize_response.response == 2

    def test_access_denied(self, portals, dbus_con, xdp_app_info):
        app_id = xdp_app_info.app_id
        synth_intf = xdp.get_portal_iface(dbus_con, "Speech")

        self.set_permissions(dbus_con, app_id, ["no"])

        session = xdp.Session(
            dbus_con,
            synth_intf.CreateSession({"session_handle_token": "session_token0"}),
        )

        request = xdp.Request(dbus_con, synth_intf)
        response = request.call(
            "GetProviders",
            session_handle=session.handle,
            parent_window="window-hndl",
            options={},
        )

        assert response
        assert response.response == 1

        request = xdp.Request(dbus_con, synth_intf)
        response = request.call(
            "GetVoices",
            session_handle=session.handle,
            parent_window="window-hndl",
            provider_id="foo",
            options={},
        )

        assert response
        assert response.response == 1

        _, writefd = os.pipe()
        request = xdp.Request(dbus_con, synth_intf)
        response = request.call(
            "Synthesize",
            session_handle=session.handle,
            parent_window="window-hndl",
            provider_id="foo",
            pipe_fd=dbus.types.UnixFd(writefd),
            text="HELLO world",
            voice_id="voice-id",
            pitch=10,
            rate=10,
            is_ssml=False,
            language="en",
            options={},
        )

        assert response
        assert response.response == 1
