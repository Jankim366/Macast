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
        return app, protocol, renderer

    def test_dlna_cast_disconnects_active_airplay(self):
        app, _, _ = self.app_with_protocol("http://example/song.mp3", "PLAYING")

        app.on_dlna_cast_requested("http://example/song.mp3")

        app.airplay_manager.send_media_command.assert_called_once_with(
            "preempt")

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
