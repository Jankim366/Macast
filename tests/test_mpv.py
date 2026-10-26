import os
import subprocess
import sys
import unittest
from unittest import mock

import macast  # Initialize the package before importing the renderer.
from macast_renderer.mpv import (
    MPVRenderer,
    WINDOWS_MEDIA_CONTROL_PARAMETERS,
)


class MPVRendererTests(unittest.TestCase):

    def test_windows_media_control_parameters(self):
        self.assertIn(
            '--media-controls=yes',
            WINDOWS_MEDIA_CONTROL_PARAMETERS)
        self.assertIn(
            '--input-media-keys=no',
            WINDOWS_MEDIA_CONTROL_PARAMETERS)

    @mock.patch.object(sys, 'platform', 'win32')
    def test_windows_title_updates_the_smtc_media_title(self):
        renderer = object.__new__(MPVRenderer)
        renderer.send_command = mock.Mock()

        renderer.set_media_title('Macast DLNA Test')

        renderer.send_command.assert_has_calls([
            mock.call(['set_property', 'title', 'Macast DLNA Test']),
            mock.call([
                'set_property',
                'force-media-title',
                'Macast DLNA Test',
            ]),
        ])

    @mock.patch.object(sys, 'platform', 'win32')
    def test_windows_loadfile_includes_index_before_options(self):
        command = MPVRenderer.build_loadfile_command(
            'http://127.0.0.1/media.mp3',
            {'start': '0', 'fullscreen': 'no'})

        self.assertEqual(
            command,
            [
                'loadfile',
                'http://127.0.0.1/media.mp3',
                'replace',
                '-1',
                'start=0,fullscreen=no',
            ])

    @unittest.skipUnless(
        sys.platform == 'win32',
        'Windows mpv integration is Windows-only')
    def test_bundled_mpv_supports_windows_media_controls(self):
        executable = os.path.join(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
            'bin',
            'mpv.exe')
        if not os.path.isfile(executable):
            self.skipTest('bundled mpv.exe is not present')

        result = subprocess.run(
            [executable, '--list-options'],
            check=True,
            capture_output=True,
            text=True,
            timeout=10)
        self.assertIn('--media-controls', result.stdout)
