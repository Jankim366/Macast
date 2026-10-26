# Copyright (c) 2021 by xfangfang. All Rights Reserved.
#
# Lifecycle management for an external AirPlay receiver such as
# AirPlay-Windows.

import logging
import hashlib
import os
import re
import shutil
import subprocess
import sys
import threading
import time


logger = logging.getLogger("AirPlay")
logger.setLevel(logging.INFO)


class AirPlayProcessManager:
    """Supervise an AirPlay receiver that runs in its own process."""

    MACAST_BINARY_NAME = "airplay-windows-macast.exe"
    PIPE_NAME = r"\\.\pipe\Macast-AirPlay-Control-v1"
    STATE_FILE_NAME = "macast_airplay_state.ini"

    STARTING = "starting"
    RUNNING = "running"
    RESTARTING = "restarting"
    STOPPED = "stopped"
    MISSING = "missing"
    ERROR = "error"

    def __init__(
            self,
            executable=None,
            arguments=None,
            working_dir=None,
            settings_path=None,
            log_path=None,
            receiver_name=None,
            auto_restart=True,
            hide_until_mirror=False,
            require_macast_window=False,
            media_callback=None,
            max_restart_attempts=3,
            restart_delay=1.0,
            startup_timeout=10.0,
            instance_wait_timeout=8.0,
            shutdown_timeout=5.0,
            poll_interval=0.5,
            status_callback=None,
            env=None,
            popen_factory=None):
        self._configured_executable = executable or ""
        self.executable = None
        self.arguments = list(arguments or [])
        self.working_dir = working_dir
        self.settings_path = settings_path
        self.log_path = log_path
        self.receiver_name = self.normalize_receiver_name(receiver_name)
        self.auto_restart = auto_restart
        self.hide_until_mirror = hide_until_mirror
        self.require_macast_window = require_macast_window
        self.media_callback = media_callback
        self.max_restart_attempts = max(0, int(max_restart_attempts))
        self.restart_delay = max(0.0, float(restart_delay))
        self.startup_timeout = max(1.0, float(startup_timeout))
        self.instance_wait_timeout = max(
            0.0, float(instance_wait_timeout))
        self.shutdown_timeout = max(0.1, float(shutdown_timeout))
        self.poll_interval = max(0.05, float(poll_interval))
        self.status_callback = status_callback
        self._env = env
        self._popen_factory = popen_factory or subprocess.Popen

        self._lock = threading.RLock()
        self._stop_event = threading.Event()
        self._monitor = None
        self._aspect_monitor = None
        self._process = None
        self._log_file = None
        self._windows_job = None
        self._process_started_at = None
        self._startup_ready = False
        self._startup_deadline = None
        self._mdns_registered = set()
        self._log_read_offset = 0
        self._window_shown_for_session = False
        self._window_user_visible = False
        self._window_hide_at = None
        self._video_size = None
        self._startup_window_visible_observed = False
        self._last_window_rect = None
        self._last_client_size = None
        self._desired_running = False
        self._restart_attempts = 0
        self._state = self.STOPPED
        self._message = "AirPlay receiver is stopped"
        self._media_state = {
            "title": "",
            "artist": "",
            "album": "",
            "elapsed_ms": 0,
            "duration_ms": 0,
            "rate": 0.0,
            "control": False,
            "has_cover": False,
            "cover_path": "",
        }

    @staticmethod
    def discover_executable(configured=None, search_dirs=None, env=None):
        """Find the configured AirPlay receiver or a bundled installation.

        An explicit configuration is authoritative: if it is invalid, the
        manager does not silently launch a different receiver.
        """
        env = env if env is not None else os.environ
        configured = configured or env.get("MACAST_AIRPLAY_EXE", "")
        if configured:
            return AirPlayProcessManager._resolve_path(configured)

        candidates = []
        search_dirs = list(search_dirs or [])
        search_dirs.extend([
            os.getcwd(),
            os.path.dirname(os.path.abspath(sys.argv[0])),
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        ])
        if getattr(sys, "frozen", False):
            search_dirs.append(os.path.dirname(os.path.abspath(sys.executable)))

        names = [
            "airplay-windows-macast.exe",
            "airplay-windows.exe",
            "AirPlay-Windows.exe",
        ]
        search_subdirs = [
            "",
            "bin",
            os.path.join("bin", "AirPlay-Windows"),
            "AirPlay-Windows",
        ]

        for directory in search_dirs:
            if not directory:
                continue
            for subdir in search_subdirs:
                candidates.extend(
                    os.path.join(directory, subdir, name) for name in names)

        seen = set()
        for candidate in candidates:
            normalized = os.path.normcase(os.path.abspath(
                os.path.expanduser(candidate)))
            if normalized in seen:
                continue
            seen.add(normalized)
            resolved = AirPlayProcessManager._resolve_path(candidate)
            if resolved:
                return resolved

        for name in names:
            resolved = shutil.which(name)
            if resolved:
                return resolved
        return None

    @staticmethod
    def _resolve_path(path):
        path = os.path.expandvars(os.path.expanduser(path))
        resolved = shutil.which(path)
        if resolved:
            return os.path.abspath(resolved)
        if os.path.isfile(path):
            return os.path.abspath(path)
        return None

    @staticmethod
    def normalize_receiver_name(name):
        if not name:
            return ""
        name = "".join(
            char for char in str(name).strip()
            if ord(char) >= 32 and char not in ".\\")
        if not name:
            return ""
        encoded = name.encode("utf-8")
        if len(encoded) <= 50:
            return name
        while name and len(name.encode("utf-8")) > 50:
            name = name[:-1]
        return name.rstrip()

    @property
    def state(self):
        with self._lock:
            return self._state

    @property
    def message(self):
        with self._lock:
            return self._message

    @property
    def media_state(self):
        with self._lock:
            return dict(self._media_state)

    @property
    def state_path(self):
        if not self.working_dir:
            return None
        return os.path.join(self.working_dir, self.STATE_FILE_NAME)

    @property
    def pid(self):
        with self._lock:
            if self._process is None:
                return None
            return self._process.pid

    def is_running(self):
        with self._lock:
            return self._process is not None and self._process.poll() is None

    def start(self):
        """Start the receiver if it is not already running."""
        executable = self.discover_executable(
            self._configured_executable, env=self._env)
        with self._lock:
            if self._process is not None and self._process.poll() is None:
                self._set_status(self.RUNNING, "AirPlay receiver is running")
                return True
            if not executable:
                self._desired_running = False
                self._set_status(
                    self.MISSING,
                    "AirPlay-Windows executable was not found")
                return False
            if self.require_macast_window and os.path.basename(
                    executable).lower() != self.MACAST_BINARY_NAME:
                self._desired_running = False
                self._set_status(
                    self.ERROR,
                    "Macast-compatible AirPlay-Windows build was not found")
                return False
            executable = self._stage_executable_locked(executable)
            if self._external_instance_running(executable):
                self._set_status(
                    self.STARTING,
                    "Waiting for the previous AirPlay instance to exit")
                deadline = time.monotonic() + self.instance_wait_timeout
                while self._external_instance_running(executable) and \
                        time.monotonic() < deadline:
                    time.sleep(0.2)
                if self._external_instance_running(executable):
                    self._desired_running = False
                    self._set_status(
                        self.ERROR,
                        "AirPlay-Windows is already running outside Macast")
                    return False
            if self.require_macast_window and \
                    not self._ensure_firewall_locked(executable):
                self._desired_running = False
                self._set_status(
                    self.ERROR,
                    "Windows Firewall permission is required for AirPlay")
                return False

            self.executable = executable
            self._desired_running = True
            self._restart_attempts = 0
            self._video_size = None
            self._startup_window_visible_observed = False
            self._last_window_rect = None
            self._last_client_size = None
            self._stop_event.clear()
            self._set_status(self.STARTING, "Starting AirPlay receiver")
            try:
                self._prepare_receiver_settings_locked()
                self._start_process_locked()
            except Exception as error:
                logger.exception("Unable to start AirPlay receiver")
                self._desired_running = False
                self._set_status(self.ERROR, str(error))
                return False
            self._ensure_monitor_locked()
            self._ensure_aspect_monitor_locked()
            if self._startup_ready:
                self._set_status(
                    self.RUNNING, "AirPlay receiver is running")
            else:
                self._set_status(
                    self.STARTING,
                    "Waiting for AirPlay discovery to become ready")
            return True

    @staticmethod
    def _file_sha256(path):
        digest = hashlib.sha256()
        with open(path, "rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        return digest.hexdigest()

    def _stage_executable_locked(self, source):
        """Keep the PyInstaller backend at a stable path for firewall rules."""
        if sys.platform != "win32" or not getattr(sys, "frozen", False) or \
                not self.working_dir:
            return source
        os.makedirs(self.working_dir, exist_ok=True)
        destination = os.path.join(
            self.working_dir, self.MACAST_BINARY_NAME)
        if os.path.normcase(os.path.abspath(source)) == \
                os.path.normcase(os.path.abspath(destination)):
            return destination

        source_hash = self._file_sha256(source)
        marker = destination + ".sha256"
        try:
            if os.path.isfile(destination) and os.path.isfile(marker):
                with open(marker, "r", encoding="ascii") as stream:
                    if stream.read().strip() == source_hash:
                        return destination
        except OSError:
            pass

        temporary = destination + ".new"
        shutil.copyfile(source, temporary)
        os.replace(temporary, destination)
        with open(marker, "w", encoding="ascii", newline="\n") as stream:
            stream.write(source_hash + "\n")
        logger.info("Staged AirPlay receiver at %s", destination)
        return destination

    @staticmethod
    def _run_firewall_helper(executable, elevated):
        import ctypes
        from ctypes import wintypes

        if not elevated:
            startupinfo = subprocess.STARTUPINFO()
            startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            startupinfo.wShowWindow = 0
            try:
                return subprocess.run(
                    [executable, "--configure-firewall"],
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    timeout=15,
                    startupinfo=startupinfo).returncode
            except (OSError, subprocess.SubprocessError):
                return -1

        class ShellExecuteInfo(ctypes.Structure):
            _fields_ = [
                ("cbSize", wintypes.DWORD),
                ("fMask", ctypes.c_ulong),
                ("hwnd", wintypes.HWND),
                ("lpVerb", wintypes.LPCWSTR),
                ("lpFile", wintypes.LPCWSTR),
                ("lpParameters", wintypes.LPCWSTR),
                ("lpDirectory", wintypes.LPCWSTR),
                ("nShow", ctypes.c_int),
                ("hInstApp", wintypes.HINSTANCE),
                ("lpIDList", wintypes.LPVOID),
                ("lpClass", wintypes.LPCWSTR),
                ("hkeyClass", wintypes.HKEY),
                ("dwHotKey", wintypes.DWORD),
                ("hIcon", wintypes.HANDLE),
                ("hProcess", wintypes.HANDLE),
            ]

        shell32 = ctypes.WinDLL("shell32", use_last_error=True)
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        shell32.ShellExecuteExW.argtypes = [
            ctypes.POINTER(ShellExecuteInfo)]
        shell32.ShellExecuteExW.restype = wintypes.BOOL
        kernel32.WaitForSingleObject.argtypes = [
            wintypes.HANDLE, wintypes.DWORD]
        kernel32.WaitForSingleObject.restype = wintypes.DWORD
        kernel32.GetExitCodeProcess.argtypes = [
            wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        kernel32.GetExitCodeProcess.restype = wintypes.BOOL
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel32.CloseHandle.restype = wintypes.BOOL

        info = ShellExecuteInfo()
        info.cbSize = ctypes.sizeof(info)
        info.fMask = 0x00000040 | 0x00000100 | 0x00000400
        info.lpVerb = "runas"
        info.lpFile = executable
        info.lpParameters = "--configure-firewall"
        info.lpDirectory = os.path.dirname(executable)
        info.nShow = 0
        if not shell32.ShellExecuteExW(ctypes.byref(info)):
            return -1
        if not info.hProcess:
            return -1
        try:
            if kernel32.WaitForSingleObject(info.hProcess, 60000) != 0:
                return -1
            code = wintypes.DWORD(1)
            if not kernel32.GetExitCodeProcess(
                    info.hProcess, ctypes.byref(code)):
                return -1
            return int(code.value)
        finally:
            kernel32.CloseHandle(info.hProcess)

    def _ensure_firewall_locked(self, executable):
        if sys.platform != "win32":
            return True
        firewall_identity = os.path.normcase(
            os.path.abspath(executable))
        marker = executable + ".firewall.allowed"
        try:
            with open(marker, "r", encoding="ascii") as stream:
                if stream.read().strip() == firewall_identity:
                    return True
        except OSError:
            pass

        result = self._run_firewall_helper(executable, elevated=False)
        if result != 0:
            result = self._run_firewall_helper(executable, elevated=True)
        if result != 0:
            logger.error(
                "Unable to configure Windows Firewall for AirPlay: %s",
                result)
            return False
        with open(marker, "w", encoding="ascii", newline="\n") as stream:
            stream.write(firewall_identity + "\n")
        return True

    def restart(self):
        """Restart the receiver, even when it is currently stopped."""
        self.stop()
        return self.start()

    def stop(self, timeout=None):
        """Request a graceful close, then terminate the child if needed."""
        timeout = self.shutdown_timeout if timeout is None else timeout
        with self._lock:
            self._desired_running = False
            process = self._process
            self._stop_event.set()

        if process is not None and process.poll() is None:
            self._request_close(process)
            try:
                process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                logger.warning("AirPlay receiver did not close in time")
                process.terminate()
                try:
                    process.wait(timeout=min(2.0, timeout))
                except subprocess.TimeoutExpired:
                    logger.warning("Force-killing AirPlay receiver")
                    process.kill()
                    process.wait()

        with self._lock:
            if self._process is process:
                self._process = None
                self._close_log_locked()
            self._restart_attempts = 0
            self._window_shown_for_session = False
            self._window_user_visible = False
            self._window_hide_at = None
            self._set_status(self.STOPPED, "AirPlay receiver is stopped")
            monitor = self._monitor
            self._monitor = None
            aspect_monitor = self._aspect_monitor
            self._aspect_monitor = None

        if monitor is not None and monitor is not threading.current_thread():
            monitor.join(timeout=max(1.0, self.poll_interval * 2))
        if aspect_monitor is not None and \
                aspect_monitor is not threading.current_thread():
            aspect_monitor.join(timeout=1.0)
        with self._lock:
            self._close_windows_job_locked()
        return True

    def show_window(self):
        """Restore the receiver window on platforms we can address it."""
        if not self.is_running():
            return False
        if sys.platform != "win32":
            return False
        with self._lock:
            self._window_user_visible = True
            self._window_hide_at = None
            shown = self._restore_windows_windows(self.pid)
            if shown and self._video_size:
                self._resize_windows_video(self.pid, *self._video_size)
            return shown

    def send_media_command(self, command, position_ms=None):
        if sys.platform != "win32" or not self.is_running():
            return False
        if command not in {
                "play", "pause", "playpause",
                "previtem", "nextitem",
                "volumeup", "volumedown", "mutetoggle", "seek"}:
            return False
        payload = command
        if command == "seek":
            payload = "seek:{}".format(max(0, int(position_ms or 0)))
        payload = (payload + "\n").encode("ascii")

        import ctypes
        from ctypes import wintypes

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.WaitNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD]
        kernel32.WaitNamedPipeW.restype = wintypes.BOOL
        kernel32.CreateFileW.argtypes = [
            wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
            wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD,
            wintypes.HANDLE]
        kernel32.CreateFileW.restype = wintypes.HANDLE
        kernel32.WriteFile.argtypes = [
            wintypes.HANDLE, wintypes.LPCVOID, wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
        kernel32.WriteFile.restype = wintypes.BOOL
        kernel32.ReadFile.argtypes = [
            wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
        kernel32.ReadFile.restype = wintypes.BOOL
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel32.CloseHandle.restype = wintypes.BOOL

        if not kernel32.WaitNamedPipeW(self.PIPE_NAME, 300):
            return False
        handle = kernel32.CreateFileW(
            self.PIPE_NAME, 0x80000000 | 0x40000000, 0,
            None, 3, 0, None)
        if handle == wintypes.HANDLE(-1).value:
            return False
        try:
            written = wintypes.DWORD()
            if not kernel32.WriteFile(
                    handle, payload, len(payload),
                    ctypes.byref(written), None):
                return False
            buffer = ctypes.create_string_buffer(16)
            read = wintypes.DWORD()
            if not kernel32.ReadFile(
                    handle, buffer, len(buffer),
                    ctypes.byref(read), None):
                return False
            reply = buffer.raw[:read.value].decode(
                "ascii", errors="replace").strip()
            return reply == "OK"
        finally:
            kernel32.CloseHandle(handle)

    @staticmethod
    def _external_instance_running(executable=None):
        if sys.platform != "win32":
            return False
        if executable and "airplay-windows" not in os.path.basename(
                executable).lower():
            return False

        import ctypes
        from ctypes import wintypes

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.OpenMutexW.argtypes = [
            wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
        kernel32.OpenMutexW.restype = wintypes.HANDLE
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel32.CloseHandle.restype = wintypes.BOOL
        handle = kernel32.OpenMutexW(
            0x00100000, False, "Local\\AirPlay-Windows.Instance.v1")
        if not handle:
            return False
        kernel32.CloseHandle(handle)
        return True

    def _start_process_locked(self):
        self._startup_ready = not self.require_macast_window
        self._mdns_registered.clear()
        self._startup_deadline = (
            None if self._startup_ready
            else time.monotonic() + self.startup_timeout)
        command = [self.executable] + self.arguments
        env = self._env if self._env is not None else dict(os.environ)
        if self.working_dir:
            os.makedirs(self.working_dir, exist_ok=True)
        if self.log_path:
            os.makedirs(os.path.dirname(os.path.abspath(self.log_path)),
                        exist_ok=True)
            self._log_file = open(
                self.log_path, "a", encoding="utf-8", buffering=1)
            stdout = self._log_file
            stderr = subprocess.STDOUT
            self._log_read_offset = os.path.getsize(self.log_path)
        else:
            stdout = subprocess.DEVNULL
            stderr = subprocess.DEVNULL

        logger.info("Starting AirPlay receiver: %s", command)
        try:
            startupinfo = None
            if sys.platform == "win32":
                startupinfo = subprocess.STARTUPINFO()
                startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
                startupinfo.wShowWindow = 0
            self._process = self._popen_factory(
                command,
                stdin=subprocess.DEVNULL,
                stdout=stdout,
                stderr=stderr,
                env=env,
                cwd=self.working_dir,
                startupinfo=startupinfo)
            self._process_started_at = time.monotonic()
            if self.hide_until_mirror:
                threading.Thread(
                    target=self._hide_receiver_window,
                    args=(self._process.pid, 3.0, 0.005),
                    name="AIRPLAY_WINDOW_HIDE",
                    daemon=True).start()
            self._assign_windows_job_locked()
        except Exception:
            self._close_log_locked()
            raise

    def _prepare_receiver_settings_locked(self):
        if not self.settings_path:
            return

        settings_path = os.path.abspath(self.settings_path)
        os.makedirs(os.path.dirname(settings_path), exist_ok=True)
        lines = []
        if os.path.isfile(settings_path):
            with open(settings_path, "r", encoding="utf-8",
                      errors="replace") as stream:
                lines = stream.read().splitlines()

        values = {"chromeless": "1" if self.hide_until_mirror else "0"}
        if self.receiver_name:
            values["receiver_name"] = self.receiver_name
        found = set()
        updated = []
        for line in lines:
            key = line.split("=", 1)[0].strip()
            if key in values:
                updated.append("{}={}".format(key, values[key]))
                found.add(key)
            else:
                updated.append(line)
        for key, value in values.items():
            if key not in found:
                updated.append("{}={}".format(key, value))

        temporary = settings_path + ".macast.tmp"
        with open(temporary, "w", encoding="utf-8", newline="\n") as stream:
            stream.write("\n".join(updated) + "\n")
        os.replace(temporary, settings_path)

    def _hide_receiver_window(self, pid, timeout=2.0, interval=0.05):
        if sys.platform != "win32":
            return False
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self._any_windows_window_visible(pid):
                self._startup_window_visible_observed = True
            if self._hide_all_windows_windows(pid):
                self._ensure_windows_frame(pid, self.receiver_name)
                return True
            time.sleep(interval)
        return False

    def _update_receiver_window_locked(self, process):
        if sys.platform != "win32" or not self.hide_until_mirror:
            return

        if self._window_hide_at is not None and \
                time.monotonic() >= self._window_hide_at:
            self._hide_windows_windows(process.pid)
            self._window_hide_at = None
        elif not self._window_shown_for_session and \
                not self._window_user_visible:
            self._hide_windows_windows(process.pid)

    def _read_mirror_events_locked(self, process=None):
        if not self.log_path or not os.path.isfile(self.log_path):
            return
        try:
            with open(self.log_path, "rb") as stream:
                stream.seek(self._log_read_offset)
                data = stream.read()
                self._log_read_offset = stream.tell()
        except OSError:
            return
        if not data:
            return

        text = data.decode("utf-8", errors="replace")
        for line in text.splitlines():
            if process is not None and \
                    "mDNS register:" in line and "status=0" in line:
                if "._raop._tcp.local" in line:
                    self._mdns_registered.add("raop")
                if "._airplay._tcp.local" in line:
                    self._mdns_registered.add("airplay")
                if {"raop", "airplay"}.issubset(
                        self._mdns_registered) and not self._startup_ready:
                    self._startup_ready = True
                    self._startup_deadline = None
                    if self._desired_running:
                        self._set_status(
                            self.RUNNING,
                            "AirPlay receiver is running")
            if process is not None and \
                    "AudioReceiver: SDL audio output unavailable" in line:
                logger.warning(
                    "AirPlay receiver started without an audio output")
            if "mirror: iOS connected from" in line:
                self._window_shown_for_session = True
                self._window_user_visible = False
                self._window_hide_at = None
                self._restore_windows_windows(self.pid)
                if self._video_size:
                    self._resize_windows_video(self.pid, *self._video_size)
            elif "mirror stream ended:" in line or \
                    "TEARDOWN full session=" in line or \
                    "Connection ended: cleared active session=" in line:
                self._window_shown_for_session = False
                self._window_user_visible = False
                self._window_hide_at = time.monotonic() + 2.0
                self._video_size = None
            else:
                match = re.search(
                    r"mirror video:.*?(\d+)x(\d+)", line)
                if not match:
                    match = re.search(
                        r"decoded first frame: (\d+)x(\d+)", line)
                if not match:
                    match = re.search(
                        r"decoded \d+ frames \((\d+)x(\d+)\)", line)
                if match:
                    size = (int(match.group(1)), int(match.group(2)))
                    if size != self._video_size:
                        self._video_size = size
                        if self._window_shown_for_session or \
                                self._window_user_visible:
                            self._resize_windows_video(self.pid, *size)

    def _ensure_monitor_locked(self):
        if self._monitor is not None and self._monitor.is_alive():
            return
        self._monitor = threading.Thread(
            target=self._monitor_process,
            name="AIRPLAY_PROCESS_MONITOR",
            daemon=True)
        self._monitor.start()

    def _ensure_aspect_monitor_locked(self):
        if self._aspect_monitor is not None and \
                self._aspect_monitor.is_alive():
            return
        self._aspect_monitor = threading.Thread(
            target=self._monitor_aspect_ratio,
            name="AIRPLAY_ASPECT_MONITOR",
            daemon=True)
        self._aspect_monitor.start()

    def _monitor_aspect_ratio(self):
        while not self._stop_event.wait(0.03):
            with self._lock:
                process = self._process
                visible = self._window_shown_for_session or \
                    self._window_user_visible
                video_size = self._video_size
            if process is None or not visible or not video_size:
                continue
            try:
                self._ensure_windows_frame(process.pid, self.receiver_name)
                self._enforce_windows_video(process.pid, *video_size)
            except Exception:
                logger.exception("Unable to enforce AirPlay window aspect")

    def _monitor_process(self):
        while not self._stop_event.wait(self.poll_interval):
            with self._lock:
                process = self._process
                desired = self._desired_running
            if process is None or not desired:
                continue

            return_code = process.poll()
            if return_code is None:
                media_changed = False
                with self._lock:
                    self._read_mirror_events_locked(process)
                    self._update_receiver_window_locked(process)
                    media_changed = self._read_media_state_locked()
                    startup_timed_out = (
                        not self._startup_ready and
                        self._startup_deadline is not None and
                        time.monotonic() >= self._startup_deadline)
                    if startup_timed_out:
                        self._startup_deadline = None
                if media_changed:
                    callback = self.media_callback
                    if callback is not None:
                        try:
                            callback(self.media_state)
                        except Exception:
                            logger.exception("AirPlay media callback failed")
                if startup_timed_out:
                    logger.error(
                        "AirPlay receiver did not publish mDNS services "
                        "before the startup timeout")
                    if self.auto_restart and \
                            self._restart_attempts < \
                            self.max_restart_attempts:
                        with self._lock:
                            self._restart_attempts += 1
                            self._set_status(
                                self.RESTARTING,
                                "AirPlay discovery failed; restarting "
                                "({}/{})".format(
                                    self._restart_attempts,
                                    self.max_restart_attempts))
                        process.terminate()
                    else:
                        with self._lock:
                            self._desired_running = False
                            self._set_status(
                                self.ERROR,
                                "AirPlay started, but discovery did not "
                                "become ready")
                if self._process_started_at is not None and \
                        time.monotonic() - self._process_started_at >= 30:
                    self._restart_attempts = 0
                continue

            with self._lock:
                if self._process is process:
                    self._process = None
                    self._close_log_locked()
                desired = self._desired_running

            logger.warning("AirPlay receiver exited with code %s", return_code)
            if not desired:
                continue
            if return_code == 0 and self._startup_ready:
                with self._lock:
                    self._desired_running = False
                    self._set_status(
                        self.STOPPED,
                        "AirPlay receiver exited")
                return
            if not self.auto_restart or \
                    self._restart_attempts >= self.max_restart_attempts:
                with self._lock:
                    self._desired_running = False
                    self._set_status(
                        self.ERROR,
                        "AirPlay receiver exited with code {}".format(
                            return_code))
                return

            self._restart_attempts += 1
            with self._lock:
                self._set_status(
                    self.RESTARTING,
                    "Restarting AirPlay receiver ({}/{})".format(
                        self._restart_attempts, self.max_restart_attempts))
            if self._stop_event.wait(self.restart_delay):
                return

            with self._lock:
                if not self._desired_running:
                    return
                try:
                    self._start_process_locked()
                except Exception as error:
                    logger.exception("Unable to restart AirPlay receiver")
                    self._desired_running = False
                    self._set_status(self.ERROR, str(error))
                    return
                if self._startup_ready:
                    self._set_status(
                        self.RUNNING, "AirPlay receiver is running")
                else:
                    self._set_status(
                        self.STARTING,
                        "Waiting for AirPlay discovery to become ready")

    def _read_media_state_locked(self):
        path = self.state_path
        if not path or not os.path.isfile(path):
            return False
        values = {}
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as stream:
                for line in stream:
                    key, separator, value = line.rstrip("\r\n").partition("=")
                    if separator:
                        values[key] = value
        except OSError:
            return False

        state = dict(self._media_state)
        for key in ("title", "artist", "album"):
            encoded = values.get(key + "_hex", "")
            try:
                state[key] = bytes.fromhex(encoded).decode(
                    "utf-8", errors="replace") if encoded else ""
            except ValueError:
                state[key] = ""

        for key in ("elapsed_ms", "duration_ms"):
            try:
                state[key] = max(0, int(values.get(key, state[key])))
            except (TypeError, ValueError):
                pass
        try:
            state["rate"] = float(values.get("rate", state["rate"]))
        except (TypeError, ValueError):
            pass
        state["control"] = values.get("control") == "1"
        state["has_cover"] = values.get("has_cover") == "1"
        state["cover_path"] = values.get("cover_path", "")
        if state["cover_path"] and self.working_dir and \
                not os.path.isabs(state["cover_path"]):
            state["cover_path"] = os.path.join(
                self.working_dir, state["cover_path"])
        if state == self._media_state:
            return False
        self._media_state = state
        return True

    def _set_status(self, state, message):
        self._state = state
        self._message = message
        callback = self.status_callback
        if callback is not None:
            try:
                callback(state, message)
            except Exception:
                logger.exception("AirPlay status callback failed")

    def _close_log_locked(self):
        if self._log_file is not None:
            try:
                self._log_file.close()
            except Exception:
                logger.exception("Unable to close AirPlay log")
            self._log_file = None

    def _assign_windows_job_locked(self):
        if sys.platform != "win32":
            return
        try:
            if self._windows_job is None:
                self._windows_job = self._create_windows_job()
            if self._windows_job is None:
                return

            import ctypes
            from ctypes import wintypes

            kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel32.OpenProcess.argtypes = [
                wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
            kernel32.OpenProcess.restype = wintypes.HANDLE
            kernel32.AssignProcessToJobObject.argtypes = [
                wintypes.HANDLE, wintypes.HANDLE]
            kernel32.AssignProcessToJobObject.restype = wintypes.BOOL
            kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
            kernel32.CloseHandle.restype = wintypes.BOOL
            process_handle = kernel32.OpenProcess(
                0x0100 | 0x0001, False, self._process.pid)
            if not process_handle:
                logger.warning(
                    "Unable to open AirPlay child for JobObject")
                return
            try:
                if not kernel32.AssignProcessToJobObject(
                        self._windows_job, process_handle):
                    logger.warning(
                        "Unable to assign AirPlay child to JobObject: %s",
                        ctypes.get_last_error())
            finally:
                kernel32.CloseHandle(process_handle)
        except Exception:
            logger.exception("Unable to configure AirPlay JobObject")

    @staticmethod
    def _create_windows_job():
        if sys.platform != "win32":
            return None

        import ctypes
        from ctypes import wintypes

        class IoCounters(ctypes.Structure):
            _fields_ = [
                ("ReadOperationCount", ctypes.c_ulonglong),
                ("WriteOperationCount", ctypes.c_ulonglong),
                ("OtherOperationCount", ctypes.c_ulonglong),
                ("ReadTransferCount", ctypes.c_ulonglong),
                ("WriteTransferCount", ctypes.c_ulonglong),
                ("OtherTransferCount", ctypes.c_ulonglong),
            ]

        class BasicLimits(ctypes.Structure):
            _fields_ = [
                ("PerProcessUserTimeLimit", ctypes.c_longlong),
                ("PerJobUserTimeLimit", ctypes.c_longlong),
                ("LimitFlags", wintypes.DWORD),
                ("MinimumWorkingSetSize", ctypes.c_size_t),
                ("MaximumWorkingSetSize", ctypes.c_size_t),
                ("ActiveProcessLimit", wintypes.DWORD),
                ("Affinity", ctypes.c_size_t),
                ("PriorityClass", wintypes.DWORD),
                ("SchedulingClass", wintypes.DWORD),
            ]

        class ExtendedLimits(ctypes.Structure):
            _fields_ = [
                ("BasicLimitInformation", BasicLimits),
                ("IoInfo", IoCounters),
                ("ProcessMemoryLimit", ctypes.c_size_t),
                ("JobMemoryLimit", ctypes.c_size_t),
                ("PeakProcessMemoryUsed", ctypes.c_size_t),
                ("PeakJobMemoryUsed", ctypes.c_size_t),
            ]

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.CreateJobObjectW.argtypes = [
            ctypes.c_void_p, wintypes.LPCWSTR]
        kernel32.CreateJobObjectW.restype = wintypes.HANDLE
        kernel32.SetInformationJobObject.argtypes = [
            wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
        kernel32.SetInformationJobObject.restype = wintypes.BOOL
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel32.CloseHandle.restype = wintypes.BOOL
        job = kernel32.CreateJobObjectW(None, None)
        if not job:
            logger.warning(
                "Unable to create AirPlay JobObject: %s",
                ctypes.get_last_error())
            return None

        limits = ExtendedLimits()
        limits.BasicLimitInformation.LimitFlags = 0x00002000
        if not kernel32.SetInformationJobObject(
                job, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
            logger.warning(
                "Unable to configure AirPlay JobObject: %s",
                ctypes.get_last_error())
            kernel32.CloseHandle(job)
            return None
        return job

    def _close_windows_job_locked(self):
        if self._windows_job is None:
            return
        try:
            import ctypes
            from ctypes import wintypes

            kernel32 = ctypes.WinDLL("kernel32")
            kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
            kernel32.CloseHandle.restype = wintypes.BOOL
            kernel32.CloseHandle(self._windows_job)
        except Exception:
            logger.exception("Unable to close AirPlay JobObject")
        finally:
            self._windows_job = None

    @staticmethod
    def _request_close(process):
        if sys.platform == "win32":
            try:
                if AirPlayProcessManager._post_windows_close(process.pid):
                    return
            except Exception:
                logger.exception("Unable to request window close")
        try:
            process.terminate()
        except Exception:
            logger.exception("Unable to terminate AirPlay receiver")

    @staticmethod
    def _windows_windows(pid):
        import ctypes
        from ctypes import wintypes

        windows = []
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        callback_type = ctypes.WINFUNCTYPE(
            wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        user32.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
        user32.EnumWindows.restype = wintypes.BOOL
        user32.GetWindowThreadProcessId.argtypes = [
            wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
        user32.GetWindowThreadProcessId.restype = wintypes.DWORD

        def collect(hwnd, _):
            window_pid = wintypes.DWORD()
            user32.GetWindowThreadProcessId(
                hwnd, ctypes.byref(window_pid))
            if window_pid.value == pid:
                windows.append(hwnd)
            return True

        callback = callback_type(collect)
        user32.EnumWindows(callback, 0)
        return windows

    @staticmethod
    def _main_windows(pid):
        import ctypes
        from ctypes import wintypes

        windows = AirPlayProcessManager._windows_windows(pid)
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        user32.GetWindowTextLengthW.argtypes = [wintypes.HWND]
        user32.GetWindowTextLengthW.restype = ctypes.c_int
        user32.GetWindowTextW.argtypes = [
            wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
        user32.GetWindowTextW.restype = ctypes.c_int

        result = []
        for hwnd in windows:
            length = user32.GetWindowTextLengthW(hwnd)
            if length <= 0:
                continue
            title = ctypes.create_unicode_buffer(length + 1)
            user32.GetWindowTextW(hwnd, title, length + 1)
            if title.value.strip():
                result.append(hwnd)
        return result

    @staticmethod
    def _hide_windows_windows(pid):
        import ctypes
        from ctypes import wintypes

        AirPlayProcessManager._ensure_windows_frame(pid)
        windows = AirPlayProcessManager._main_windows(pid)
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
        user32.ShowWindow.restype = wintypes.BOOL
        for hwnd in windows:
            user32.ShowWindow(hwnd, 0)
        return bool(windows)

    @staticmethod
    def _hide_all_windows_windows(pid):
        import ctypes
        from ctypes import wintypes

        windows = AirPlayProcessManager._windows_windows(pid)
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
        user32.ShowWindow.restype = wintypes.BOOL
        for hwnd in windows:
            user32.ShowWindow(hwnd, 0)
        return bool(windows)

    @staticmethod
    def _any_windows_window_visible(pid):
        import ctypes
        from ctypes import wintypes

        windows = AirPlayProcessManager._windows_windows(pid)
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        user32.IsWindowVisible.argtypes = [wintypes.HWND]
        user32.IsWindowVisible.restype = wintypes.BOOL
        return any(user32.IsWindowVisible(hwnd) for hwnd in windows)

    @staticmethod
    def _windows_window_visible(pid):
        import ctypes
        from ctypes import wintypes

        windows = AirPlayProcessManager._main_windows(pid)
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        user32.IsWindowVisible.argtypes = [wintypes.HWND]
        user32.IsWindowVisible.restype = wintypes.BOOL
        return any(user32.IsWindowVisible(hwnd) for hwnd in windows)

    @staticmethod
    def _windows_window_size(pid):
        rect = AirPlayProcessManager._windows_window_rect(pid)
        if not rect:
            return None
        return (rect[2] - rect[0], rect[3] - rect[1])

    @staticmethod
    def _windows_window_rect(pid):
        import ctypes
        from ctypes import wintypes

        windows = AirPlayProcessManager._main_windows(pid)
        if not windows:
            return None
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        user32.GetWindowRect.argtypes = [
            wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
        user32.GetWindowRect.restype = wintypes.BOOL
        rect = wintypes.RECT()
        if not user32.GetWindowRect(windows[0], ctypes.byref(rect)):
            return None
        return (rect.left, rect.top, rect.right, rect.bottom)

    @staticmethod
    def _windows_client_size(pid):
        import ctypes
        from ctypes import wintypes

        windows = AirPlayProcessManager._main_windows(pid)
        if not windows:
            return None
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        user32.GetClientRect.argtypes = [
            wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
        user32.GetClientRect.restype = wintypes.BOOL
        rect = wintypes.RECT()
        if not user32.GetClientRect(windows[0], ctypes.byref(rect)):
            return None
        return (rect.right - rect.left, rect.bottom - rect.top)

    @staticmethod
    def _ensure_windows_frame(pid, title=None, force_frame=False):
        if sys.platform != "win32":
            return False
        import ctypes
        from ctypes import wintypes

        windows = AirPlayProcessManager._main_windows(pid)
        if not windows:
            return False
        hwnd = windows[0]
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        get_long = user32.GetWindowLongPtrW
        set_long = user32.SetWindowLongPtrW
        get_long.argtypes = [wintypes.HWND, ctypes.c_int]
        get_long.restype = ctypes.c_ssize_t
        set_long.argtypes = [
            wintypes.HWND, ctypes.c_int, ctypes.c_ssize_t]
        set_long.restype = ctypes.c_ssize_t
        user32.SetWindowPos.argtypes = [
            wintypes.HWND, wintypes.HWND,
            ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
            wintypes.UINT]
        user32.SetWindowPos.restype = wintypes.BOOL

        style = get_long(hwnd, -16)
        desired = style & ~0x80000000
        desired |= 0x00C00000  # WS_CAPTION
        desired |= 0x00080000  # WS_SYSMENU
        desired |= 0x00020000  # WS_MINIMIZEBOX
        desired |= 0x00010000  # WS_MAXIMIZEBOX
        desired |= 0x00040000  # WS_THICKFRAME
        if desired != style:
            set_long(hwnd, -16, desired)
        if desired != style or force_frame:
            flags = 0x0020 | 0x0002 | 0x0001 | 0x0004 | 0x0010
            user32.SetWindowPos(
                hwnd, None, 0, 0, 0, 0, flags)
        if title:
            user32.SetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPCWSTR]
            user32.SetWindowTextW.restype = wintypes.BOOL
            user32.SetWindowTextW(hwnd, title)
        return True

    @staticmethod
    def _window_frame_margins(pid):
        outer = AirPlayProcessManager._windows_window_size(pid)
        client = AirPlayProcessManager._windows_client_size(pid)
        if not outer or not client:
            return None
        return (outer[0] - client[0], outer[1] - client[1])

    @staticmethod
    def _window_client_limits(pid):
        import ctypes
        from ctypes import wintypes

        windows = AirPlayProcessManager._main_windows(pid)
        margins = AirPlayProcessManager._window_frame_margins(pid)
        if not windows or not margins:
            return None
        hwnd = windows[0]
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        user32.MonitorFromWindow.argtypes = [
            wintypes.HWND, wintypes.DWORD]
        user32.MonitorFromWindow.restype = wintypes.HANDLE
        user32.GetMonitorInfoW.argtypes = [
            wintypes.HANDLE, ctypes.c_void_p]
        user32.GetMonitorInfoW.restype = wintypes.BOOL

        class MonitorInfo(ctypes.Structure):
            _fields_ = [
                ("cbSize", wintypes.DWORD),
                ("rcMonitor", wintypes.RECT),
                ("rcWork", wintypes.RECT),
                ("dwFlags", wintypes.DWORD),
            ]

        monitor = user32.MonitorFromWindow(hwnd, 2)
        info = MonitorInfo()
        info.cbSize = ctypes.sizeof(MonitorInfo)
        if not monitor or not user32.GetMonitorInfoW(
                monitor, ctypes.byref(info)):
            return None
        work_width = info.rcWork.right - info.rcWork.left
        work_height = info.rcWork.bottom - info.rcWork.top
        return (
            max(160, int(work_width * 0.85) - margins[0]),
            max(120, int(work_height * 0.85) - margins[1]),
        )

    @staticmethod
    def fit_window_to_video(
            video_width, video_height, max_width, max_height):
        if video_width <= 0 or video_height <= 0:
            return None
        max_width = max(160, int(max_width))
        max_height = max(120, int(max_height))
        scale = min(
            float(max_width) / video_width,
            float(max_height) / video_height)
        return (
            max(160, int(video_width * scale)),
            max(120, int(video_height * scale)),
        )

    @staticmethod
    def resize_video_client_size(
            client_width, client_height,
            window_rect, last_window_rect,
            video_width, video_height):
        target_ratio = float(video_width) / video_height
        current_ratio = float(client_width) / client_height
        if abs(current_ratio - target_ratio) <= 0.004:
            return (client_width, client_height, "center")

        last_rect = last_window_rect or window_rect
        left_move = abs(window_rect[0] - last_rect[0])
        top_move = abs(window_rect[1] - last_rect[1])
        right_move = abs(window_rect[2] - last_rect[2])
        bottom_move = abs(window_rect[3] - last_rect[3])
        vertical_change = top_move + bottom_move
        horizontal_change = left_move + right_move

        if vertical_change > horizontal_change:
            client_width = max(160, int(client_height * target_ratio))
            if top_move > bottom_move:
                anchor = "bottom"
            elif bottom_move > top_move:
                anchor = "top"
            else:
                anchor = "center"
        else:
            client_height = max(120, int(client_width / target_ratio))
            if left_move > right_move:
                anchor = "right"
            elif right_move > left_move:
                anchor = "left"
            else:
                anchor = "center"
        return (client_width, client_height, anchor)

    @staticmethod
    def _set_windows_client_size(
            pid, client_width, client_height, anchor="center"):
        import ctypes
        from ctypes import wintypes

        AirPlayProcessManager._ensure_windows_frame(pid)
        windows = AirPlayProcessManager._main_windows(pid)
        margins = AirPlayProcessManager._window_frame_margins(pid)
        if not windows or not margins:
            return False
        hwnd = windows[0]
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        user32.GetWindowRect.argtypes = [
            wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
        user32.GetWindowRect.restype = wintypes.BOOL
        user32.SetWindowPos.argtypes = [
            wintypes.HWND, wintypes.HWND,
            ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
            wintypes.UINT]
        user32.SetWindowPos.restype = wintypes.BOOL
        current = wintypes.RECT()
        if not user32.GetWindowRect(hwnd, ctypes.byref(current)):
            return False

        outer_width = max(160, int(client_width) + margins[0])
        outer_height = max(120, int(client_height) + margins[1])
        current_width = current.right - current.left
        current_height = current.bottom - current.top
        if anchor == "top":
            target_x = current.left + (current_width - outer_width) // 2
            target_y = current.top
        elif anchor == "bottom":
            target_x = current.left + (current_width - outer_width) // 2
            target_y = current.bottom - outer_height
        elif anchor == "left":
            target_x = current.left
            target_y = current.top + (current_height - outer_height) // 2
        elif anchor == "right":
            target_x = current.right - outer_width
            target_y = current.top + (current_height - outer_height) // 2
        else:
            target_x = current.left + (current_width - outer_width) // 2
            target_y = current.top + (current_height - outer_height) // 2
        flags = 0x0004 | 0x0010  # SWP_NOZORDER | SWP_NOACTIVATE
        return bool(user32.SetWindowPos(
            hwnd, None, target_x, target_y,
            outer_width, outer_height, flags))

    def _enforce_windows_video(self, pid, video_width, video_height):
        if sys.platform != "win32" or video_width <= 0 or video_height <= 0:
            return False
        import ctypes
        from ctypes import wintypes

        self._ensure_windows_frame(pid)
        windows = AirPlayProcessManager._main_windows(pid)
        if not windows:
            return False
        hwnd = windows[0]
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        user32.IsIconic.argtypes = [wintypes.HWND]
        user32.IsIconic.restype = wintypes.BOOL
        user32.IsZoomed.argtypes = [wintypes.HWND]
        user32.IsZoomed.restype = wintypes.BOOL
        user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
        user32.ShowWindow.restype = wintypes.BOOL
        if user32.IsIconic(hwnd):
            return False
        if user32.IsZoomed(hwnd):
            user32.ShowWindow(hwnd, 9)
            result = AirPlayProcessManager._resize_windows_video(
                pid, video_width, video_height)
            self._last_window_rect = self._windows_window_rect(pid)
            self._last_client_size = self._windows_client_size(pid)
            return result

        client = AirPlayProcessManager._windows_client_size(pid)
        window_rect = AirPlayProcessManager._windows_window_rect(pid)
        if not client or not window_rect:
            return False
        client_width, client_height = client
        target_ratio = float(video_width) / video_height
        current_ratio = float(client_width) / client_height
        if abs(current_ratio - target_ratio) <= 0.004:
            self._last_window_rect = window_rect
            self._last_client_size = client
            return True

        last_rect = self._last_window_rect or window_rect
        client_width, client_height, anchor = self.resize_video_client_size(
            client_width, client_height,
            window_rect, last_rect,
            video_width, video_height)
        limits = self._window_client_limits(pid)
        if limits and (
                client_width > limits[0] or client_height > limits[1]):
            fitted = self.fit_window_to_video(
                video_width, video_height, limits[0], limits[1])
            if fitted:
                client_width, client_height = fitted

        result = AirPlayProcessManager._set_windows_client_size(
            pid, client_width, client_height, anchor)
        if result:
            self._last_window_rect = self._windows_window_rect(pid)
            self._last_client_size = self._windows_client_size(pid)
        return result

    @staticmethod
    def _resize_windows_video(pid, video_width, video_height):
        if sys.platform != "win32" or video_width <= 0 or video_height <= 0:
            return False

        import ctypes
        from ctypes import wintypes

        AirPlayProcessManager._ensure_windows_frame(pid)
        windows = AirPlayProcessManager._main_windows(pid)
        if not windows:
            return False
        hwnd = windows[0]
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        user32.GetWindowRect.argtypes = [
            wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
        user32.GetWindowRect.restype = wintypes.BOOL
        user32.MonitorFromWindow.argtypes = [
            wintypes.HWND, wintypes.DWORD]
        user32.MonitorFromWindow.restype = wintypes.HANDLE
        user32.SetWindowPos.argtypes = [
            wintypes.HWND, wintypes.HWND,
            ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
            wintypes.UINT]
        user32.SetWindowPos.restype = wintypes.BOOL

        class MonitorInfo(ctypes.Structure):
            _fields_ = [
                ("cbSize", wintypes.DWORD),
                ("rcMonitor", wintypes.RECT),
                ("rcWork", wintypes.RECT),
                ("dwFlags", wintypes.DWORD),
            ]

        user32.GetMonitorInfoW.argtypes = [
            wintypes.HANDLE, ctypes.POINTER(MonitorInfo)]
        user32.GetMonitorInfoW.restype = wintypes.BOOL

        current = wintypes.RECT()
        if not user32.GetWindowRect(hwnd, ctypes.byref(current)):
            return False
        monitor = user32.MonitorFromWindow(hwnd, 2)
        info = MonitorInfo()
        info.cbSize = ctypes.sizeof(MonitorInfo)
        if not monitor or not user32.GetMonitorInfoW(
                monitor, ctypes.byref(info)):
            work_width = 1920
            work_height = 1080
            work_left = 0
            work_top = 0
        else:
            work_width = info.rcWork.right - info.rcWork.left
            work_height = info.rcWork.bottom - info.rcWork.top
            work_left = info.rcWork.left
            work_top = info.rcWork.top

        margins = AirPlayProcessManager._window_frame_margins(pid)
        if not margins:
            return False
        max_width = max(160, int(work_width * 0.85) - margins[0])
        max_height = max(120, int(work_height * 0.85) - margins[1])
        target_size = AirPlayProcessManager.fit_window_to_video(
            video_width, video_height, max_width, max_height)
        if not target_size:
            return False
        target_width, target_height = target_size
        return AirPlayProcessManager._set_windows_client_size(
            pid, target_width, target_height)

    @staticmethod
    def _post_windows_close(pid):
        import ctypes
        from ctypes import wintypes

        windows = AirPlayProcessManager._main_windows(pid)
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        user32.PostMessageW.argtypes = [
            wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
        user32.PostMessageW.restype = wintypes.BOOL
        for hwnd in windows:
            user32.PostMessageW(hwnd, 0x0010, 0, 0)
        return bool(windows)

    @staticmethod
    def _restore_windows_windows(pid):
        import ctypes
        from ctypes import wintypes

        windows = AirPlayProcessManager._main_windows(pid)
        user32 = ctypes.WinDLL("user32", use_last_error=True)
        user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
        user32.ShowWindow.restype = wintypes.BOOL
        user32.SetForegroundWindow.argtypes = [wintypes.HWND]
        user32.SetForegroundWindow.restype = wintypes.BOOL
        for hwnd in windows:
            user32.ShowWindow(hwnd, 9)
            user32.SetForegroundWindow(hwnd)
        return bool(windows)
