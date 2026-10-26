import threading
import unittest
from types import SimpleNamespace
from unittest import mock

from macast.macast import Macast


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
            "rate": 1.0,
        }
        app.airplay_manager.send_media_command.return_value = True

        app.on_dlna_cast_requested("http://example/song.mp3")
        self.assertTrue(app.airplay_paused_by_dlna)
        app.airplay_manager.send_media_command.assert_called_once_with("pause")

        app.airplay_manager.send_media_command.reset_mock()
        app.resume_airplay_after_dlna()
        app.airplay_manager.send_media_command.assert_called_once_with("play")
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
