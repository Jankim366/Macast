import os
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock

import macast  # Initialize the package before importing the manager.
from macast.macast import Macast
from macast.bluetooth_audio import BluetoothAudioProcessManager


class BluetoothAudioProcessManagerTests(unittest.TestCase):

    def test_bundled_backend_is_discovered(self):
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        executable = os.path.join(
            root,
            "bin",
            "BluetoothAudio",
            "macast-bluetooth-audio.exe")
        if not os.path.isfile(executable):
            self.skipTest("bundled Bluetooth backend is not present")

        self.assertEqual(
            BluetoothAudioProcessManager.discover_executable(
                search_dirs=[root], env={}),
            executable)

    def test_state_file_decodes_utf8_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            manager = BluetoothAudioProcessManager(
                working_dir=directory)
            with open(manager.state_path, "w", encoding="utf-8") as stream:
                stream.write(
                    "version=1\n"
                    "state=connected\n"
                    "device_id_hex=4944\n"
                    "device_name_hex=E6898BE69CBA\n"
                    "error_hex=\n")

            state = manager._read_state_file()

        self.assertEqual(state["state"], "connected")
        self.assertEqual(state["device_id"], "ID")
        self.assertEqual(state["device_name"], "手机")

    def test_menu_uses_automatic_device_mode(self):
        manager = mock.Mock()
        manager.STOPPED = BluetoothAudioProcessManager.STOPPED
        manager.STARTING = BluetoothAudioProcessManager.STARTING
        manager.READY = BluetoothAudioProcessManager.READY
        manager.CONNECTING = BluetoothAudioProcessManager.CONNECTING
        manager.CONNECTED = BluetoothAudioProcessManager.CONNECTED
        manager.ERROR = BluetoothAudioProcessManager.ERROR
        manager.MISSING = BluetoothAudioProcessManager.MISSING
        manager.state = manager.READY
        manager.device_id = ""
        manager.device_name = ""
        manager.devices = [{
            "id": "device-id",
            "name": "Phone",
        }]
        manager.is_running.return_value = True
        app = object.__new__(Macast)
        app.bluetooth_audio_manager = manager

        items = Macast.build_bluetooth_audio_menu(app)

        self.assertTrue(any(
            item is not None and
            item.text == "Automatically follows connected Bluetooth audio"
            for item in items))
