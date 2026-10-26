import importlib.util
import os
import sys
import tempfile
import time
import unittest
from unittest import mock


MODULE_PATH = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "macast",
    "airplay.py")
SPEC = importlib.util.spec_from_file_location("macast_airplay", MODULE_PATH)
AIRPLAY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AIRPLAY)


class AirPlayProcessManagerTests(unittest.TestCase):

    def setUp(self):
        self.managers = []

    def tearDown(self):
        for manager in reversed(self.managers):
            manager.stop(timeout=0.2)

    def manager(self, **kwargs):
        kwargs.setdefault("executable", sys.executable)
        kwargs.setdefault("shutdown_timeout", 0.3)
        kwargs.setdefault("poll_interval", 0.05)
        kwargs.setdefault("restart_delay", 0.05)
        manager = AIRPLAY.AirPlayProcessManager(**kwargs)
        self.managers.append(manager)
        return manager

    def test_explicit_and_environment_paths_are_discovered(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = os.path.join(directory, "airplay-windows.exe")
            with open(executable, "w", encoding="utf-8") as stream:
                stream.write("test")

            self.assertEqual(
                AIRPLAY.AirPlayProcessManager.discover_executable(
                    executable, env={}),
                os.path.abspath(executable))
            self.assertEqual(
                AIRPLAY.AirPlayProcessManager.discover_executable(
                    env={"MACAST_AIRPLAY_EXE": executable}),
                os.path.abspath(executable))

    def test_bundled_airplay_windows_directory_is_discovered(self):
        with tempfile.TemporaryDirectory() as directory:
            app_dir = os.path.join(directory, "bin", "AirPlay-Windows")
            os.makedirs(app_dir)
            executable = os.path.join(app_dir, "airplay-windows.exe")
            with open(executable, "w", encoding="utf-8") as stream:
                stream.write("test")

            self.assertEqual(
                AIRPLAY.AirPlayProcessManager.discover_executable(
                    search_dirs=[directory], env={}),
                os.path.abspath(executable))

    def test_macast_airplay_windows_build_is_preferred(self):
        with tempfile.TemporaryDirectory() as directory:
            custom = os.path.join(
                directory, "airplay-windows-macast.exe")
            official = os.path.join(directory, "airplay-windows.exe")
            for path in (custom, official):
                with open(path, "w", encoding="utf-8") as stream:
                    stream.write("test")

            self.assertEqual(
                AIRPLAY.AirPlayProcessManager.discover_executable(
                    search_dirs=[directory], env={}),
                os.path.abspath(custom))

    def test_missing_executable_fails_without_launching(self):
        manager = self.manager(executable="this-file-does-not-exist")
        self.assertFalse(manager.start())
        self.assertEqual(manager.state, manager.MISSING)
        self.assertFalse(manager.is_running())

    def test_standard_window_build_is_required_in_macast_mode(self):
        manager = self.manager(
            executable=sys.executable,
            require_macast_window=True)
        self.assertFalse(manager.start())
        self.assertEqual(manager.state, manager.ERROR)

    def test_start_and_stop_real_child_process(self):
        manager = self.manager(
            arguments=["-c", "import time; time.sleep(60)"],
            env=dict(os.environ))
        self.assertTrue(manager.start())
        self.assertTrue(manager.is_running())
        self.assertIsNotNone(manager.pid)
        self.assertTrue(manager.stop())
        self.assertFalse(manager.is_running())
        self.assertEqual(manager.state, manager.STOPPED)

    def test_restart_replaces_child_process(self):
        manager = self.manager(
            arguments=["-c", "import time; time.sleep(60)"],
            env=dict(os.environ))
        self.assertTrue(manager.start())
        first_pid = manager.pid
        self.assertTrue(manager.restart())
        self.assertTrue(manager.is_running())
        self.assertNotEqual(first_pid, manager.pid)

    def test_working_directory_preserves_receiver_identity_location(self):
        with tempfile.TemporaryDirectory() as directory:
            manager = self.manager(
                arguments=[
                    "-c",
                    "import os, pathlib; "
                    "pathlib.Path('cwd.txt').write_text(os.getcwd())"],
                working_dir=directory,
                env=dict(os.environ))
            self.assertTrue(manager.start())

            deadline = time.time() + 2
            cwd_file = os.path.join(directory, "cwd.txt")
            while time.time() < deadline and not os.path.exists(cwd_file):
                time.sleep(0.05)

            with open(cwd_file, "r", encoding="utf-8") as stream:
                self.assertEqual(
                    os.path.normcase(stream.read()),
                    os.path.normcase(os.path.abspath(directory)))

    def test_media_state_file_is_parsed(self):
        with tempfile.TemporaryDirectory() as directory:
            manager = self.manager(working_dir=directory)
            with open(manager.state_path, "w", encoding="utf-8") as stream:
                stream.write(
                    "title_hex={}\n"
                    "artist_hex={}\n"
                    "album_hex={}\n"
                    "elapsed_ms=65000\n"
                    "duration_ms=180000\n"
                    "rate=1\n"
                    "control=1\n"
                    "has_cover=1\n"
                    "cover_path=cover.jpg\n".format(
                        "AirPlay Song".encode("utf-8").hex().upper(),
                        "Macast Artist".encode("utf-8").hex().upper(),
                        "Album".encode("utf-8").hex().upper()))

            self.assertTrue(manager._read_media_state_locked())
            state = manager.media_state
            self.assertEqual(state["title"], "AirPlay Song")
            self.assertEqual(state["artist"], "Macast Artist")
            self.assertEqual(state["elapsed_ms"], 65000)
            self.assertEqual(state["duration_ms"], 180000)
            self.assertTrue(state["control"])
            self.assertTrue(state["cover_path"].endswith("cover.jpg"))

    def test_hide_mode_preserves_settings_and_enables_chromeless(self):
        with tempfile.TemporaryDirectory() as directory:
            settings_path = os.path.join(directory, "settings.ini")
            with open(settings_path, "w", encoding="utf-8") as stream:
                stream.write(
                    "receiver_name=Living Room\n"
                    "chromeless=0\n")

            manager = self.manager(
                hide_until_mirror=True,
                settings_path=settings_path,
                receiver_name="Macast.Test\\Room")
            manager._prepare_receiver_settings_locked()

            with open(settings_path, "r", encoding="utf-8") as stream:
                settings = stream.read()
            self.assertIn("receiver_name=MacastTestRoom", settings)
            self.assertIn("chromeless=1", settings)

    def test_mirror_log_events_control_window_state(self):
        with tempfile.TemporaryDirectory() as directory:
            log_path = os.path.join(directory, "airplay.log")
            manager = self.manager(
                log_path=log_path,
                hide_until_mirror=True)

            with mock.patch.object(
                    AIRPLAY.AirPlayProcessManager,
                    "_restore_windows_windows",
                    return_value=True):
                with open(log_path, "a", encoding="utf-8") as stream:
                    stream.write(
                        "mirror: iOS connected from 192.168.1.2\n"
                        "decoded first frame: 1179x2556\n")
                manager._read_mirror_events_locked()
                self.assertTrue(manager._window_shown_for_session)
                self.assertEqual(manager._video_size, (1179, 2556))

                with open(log_path, "a", encoding="utf-8") as stream:
                    stream.write("mirror stream ended: 42 frames\n")
                manager._read_mirror_events_locked()
                self.assertFalse(manager._window_shown_for_session)
                self.assertIsNotNone(manager._window_hide_at)

            self.assertEqual(
                manager.normalize_receiver_name("Macast.Test\\Room"),
                "MacastTestRoom")

    def test_mdns_registration_marks_startup_ready(self):
        with tempfile.TemporaryDirectory() as directory:
            log_path = os.path.join(directory, "airplay.log")
            manager = self.manager(log_path=log_path)
            manager._desired_running = True
            manager._state = manager.STARTING
            manager._startup_ready = False
            manager._startup_deadline = time.monotonic() + 10

            with open(log_path, "a", encoding="utf-8") as stream:
                stream.write(
                    "mDNS register: device._raop._tcp.local status=0\n"
                    "mDNS register: device._airplay._tcp.local status=0\n")

            manager._read_mirror_events_locked(object())
            self.assertTrue(manager._startup_ready)
            self.assertEqual(manager.state, manager.RUNNING)

    def test_video_window_size_matches_portrait_aspect_ratio(self):
        width, height = AIRPLAY.AirPlayProcessManager.fit_window_to_video(
            1179, 2556, 1632, 918)
        self.assertLessEqual(width, 1632)
        self.assertLessEqual(height, 918)
        self.assertAlmostEqual(
            width / float(height), 1179 / 2556.0, places=2)

    def test_vertical_edge_drag_keeps_the_new_height(self):
        manager = AIRPLAY.AirPlayProcessManager
        last_rect = (100, 100, 549, 1079)

        width, height, anchor = manager.resize_video_client_size(
            433, 1060, (100, 20, 549, 1079),
            last_rect, 554, 1200)
        self.assertEqual(height, 1060)
        self.assertGreater(width, 433)
        self.assertEqual(anchor, "bottom")

        width, height, anchor = manager.resize_video_client_size(
            433, 1060, (100, 100, 549, 1199),
            last_rect, 554, 1200)
        self.assertEqual(height, 1060)
        self.assertGreater(width, 433)
        self.assertEqual(anchor, "top")

    def test_horizontal_edge_drag_keeps_the_new_width(self):
        width, height, anchor = \
            AIRPLAY.AirPlayProcessManager.resize_video_client_size(
                520, 940, (20, 100, 549, 1079),
                (100, 100, 549, 1079), 554, 1200)
        self.assertEqual(width, 520)
        self.assertGreater(height, 940)
        self.assertEqual(anchor, "right")

    def test_unexpected_exit_restarts_then_reports_error(self):
        states = []
        manager = self.manager(
            arguments=[
                "-c",
                "import sys, time; time.sleep(0.1); sys.exit(3)"],
            env=dict(os.environ),
            max_restart_attempts=1,
            status_callback=lambda state, _: states.append(state))
        self.assertTrue(manager.start())

        deadline = time.time() + 3
        while time.time() < deadline and manager.state != manager.ERROR:
            time.sleep(0.05)

        self.assertEqual(manager.state, manager.ERROR)
        self.assertIn(manager.RESTARTING, states)
        self.assertFalse(manager.is_running())

    def test_clean_child_exit_is_not_restarted(self):
        manager = self.manager(
            arguments=["-c", "import time; time.sleep(0.1)"],
            env=dict(os.environ),
            max_restart_attempts=3)
        self.assertTrue(manager.start())

        deadline = time.time() + 2
        while time.time() < deadline and manager.state != manager.STOPPED:
            time.sleep(0.05)

        self.assertEqual(manager.state, manager.STOPPED)
        self.assertFalse(manager.is_running())


if __name__ == "__main__":
    unittest.main()
