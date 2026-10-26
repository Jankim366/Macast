import threading
import unittest
from types import SimpleNamespace
from unittest import mock

from macast.macast import Macast
from macast.bluetooth_audio import BluetoothAudioProcessManager


class PreemptionTests(unittest.TestCase):

    def app_with_protocol(self, uri, transport):
        protocol = mock.Mock()
        protocol.get_state_url.return_value = uri
        protocol.get_state_transport_state.return_value = transport
        renderer = mock.Mock()
        app = object.__new__(Macast)
        app.service = SimpleNamespace(
            protocol=protocol,
            renderer=renderer)
        app.airplay_manager = mock.Mock()
        app.airplay_manager.media_state = {
            "control": False,
            "rate": 0,
        }
        app.airplay_preemption_lock = threading.RLock()
        app.airplay_paused_by_dlna = False
        app.airplay_resume_timer = None
        app.bluetooth_owns_output = False
        app.bluetooth_audio_manager = None
        app.bluetooth_audio_menuitem = None
        app.bluetooth_audio_status_menuitem = None
        app.bluetooth_audio_toggle_menuitem = None
        app.bluetooth_audio_restart_menuitem = None
        app.bluetooth_audio_device_menuitems = []
        return app, protocol, renderer

    def test_dlna_cast_disconnects_active_airplay(self):
        app, _, _ = self.app_with_protocol("http://example/song.mp3", "PLAYING")

        app.on_dlna_cast_requested("http://example/song.mp3")

        app.airplay_manager.send_media_command.assert_called_once_with(
            "preempt")

    def test_dlna_cast_pauses_and_resumes_airplay_audio(self):
        app, _, _ = self.app_with_protocol("http://example/song.mp3", "PLAYING")
        app.airplay_manager.media_state = {
            "control": True,
            "rate": 0,
            "title": "AirPlay Song",
            "artist": "AirPlay Artist",
            "duration_ms": 180000,
        }
        app.airplay_manager.send_media_command.return_value = True

        app.on_dlna_cast_requested("http://example/song.mp3")
        self.assertTrue(app.airplay_paused_by_dlna)
        app.airplay_manager.send_media_command.assert_called_once_with("pause")

        app.airplay_manager.send_media_command.reset_mock()
        app.airplay_manager.send_media_command.side_effect = [False, True]
        app.resume_airplay_after_dlna()
        app.airplay_manager.send_media_command.assert_has_calls([
            mock.call("play"),
            mock.call("playpause"),
        ])
        self.assertFalse(app.airplay_paused_by_dlna)

    def test_airplay_session_stops_active_dlna(self):
        app, protocol, renderer = self.app_with_protocol(
            "http://example/song.mp3", "PLAYING")

        with mock.patch("macast.macast.cherrypy.engine.publish") as publish:
            app.on_airplay_session_started()

        renderer.set_media_stop.assert_called_once_with()
        protocol.set_state_stop.assert_called_once_with()
        publish.assert_called_once_with("renderer_av_stop")

    def test_airplay_session_does_not_touch_idle_dlna(self):
        app, _, renderer = self.app_with_protocol("", "NO_MEDIA_PRESENT")

        app.on_airplay_session_started()

        renderer.set_media_stop.assert_not_called()

    def test_bluetooth_connection_preempts_airplay_and_dlna(self):
        app, protocol, renderer = self.app_with_protocol(
            "http://example/video.mp4", "PLAYING")
        manager = mock.Mock()
        manager.CONNECTED = BluetoothAudioProcessManager.CONNECTED
        manager.READY = BluetoothAudioProcessManager.READY
        manager.STOPPED = BluetoothAudioProcessManager.STOPPED
        manager.ERROR = BluetoothAudioProcessManager.ERROR
        manager.MISSING = BluetoothAudioProcessManager.MISSING
        manager.device_id = "bluetooth-device"
        manager.device_name = "Phone"
        app.bluetooth_audio_manager = manager

        with mock.patch("macast.macast.Setting.set") as save_setting:
            app.on_bluetooth_audio_state_changed({
                "state": manager.CONNECTED,
                "message": "connected",
            })

        app.airplay_manager.send_media_command.assert_called_once_with(
            "preempt")
        renderer.set_media_stop.assert_called_once_with()
        protocol.set_state_stop.assert_called_once_with()
        self.assertTrue(app.bluetooth_owns_output)
        save_setting.assert_called_once()
