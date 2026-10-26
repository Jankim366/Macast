# Headless Bluetooth A2DP sink process management for Macast.

import json
import logging
import os
import shutil
import subprocess
import sys
import threading
import time


logger = logging.getLogger("BluetoothAudio")
logger.setLevel(logging.INFO)


class BluetoothAudioProcessManager:
    """Manage the hidden Windows AudioPlaybackConnection helper."""

    PIPE_NAME = r"\\.\pipe\Macast-BluetoothAudio-Control-v1"
    STATE_FILE_NAME = "macast_bluetooth_state.ini"

    STOPPED = "stopped"
    STARTING = "starting"
    READY = "ready"
    CONNECTING = "connecting"
    CONNECTED = "connected"
    ERROR = "error"
    MISSING = "missing"

    def __init__(
            self,
            executable=None,
            working_dir=None,
            state_callback=None,
            poll_interval=0.5,
            startup_timeout=5.0,
            shutdown_timeout=3.0,
            env=None,
            popen_factory=None):
        self._configured_executable = executable or ""
        self.executable = None
        self.working_dir = working_dir
        self.state_callback = state_callback
        self.poll_interval = max(0.1, float(poll_interval))
        self.startup_timeout = max(0.5, float(startup_timeout))
        self.shutdown_timeout = max(0.2, float(shutdown_timeout))
        self._env = env
        self._popen_factory = popen_factory or subprocess.Popen

        self._lock = threading.RLock()
        self._process = None
        self._monitor = None
        self._stop_event = threading.Event()
        self._state = self.STOPPED
        self._message = "Bluetooth audio receiver is stopped"
        self._device_id = ""
        self._device_name = ""
        self._error = ""
        self._devices = []
        self._last_state_signature = None

    @property
    def state_path(self):
        if not self.working_dir:
            return None
        return os.path.join(self.working_dir, self.STATE_FILE_NAME)

    @property
    def log_path(self):
        if not self.working_dir:
            return None
        return os.path.join(self.working_dir, "macast_bluetooth_audio.log")

    @property
    def state(self):
        with self._lock:
            return self._state

    @property
    def message(self):
        with self._lock:
            return self._message

    @property
    def device_id(self):
        with self._lock:
            return self._device_id

    @property
    def device_name(self):
        with self._lock:
            return self._device_name

    @property
    def devices(self):
        with self._lock:
            return list(self._devices)

    def is_running(self):
        with self._lock:
            return self._process is not None and self._process.poll() is None

    def is_connected(self):
        return self.is_running() and self.state == self.CONNECTED

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
    def discover_executable(configured=None, search_dirs=None, env=None):
        env = env if env is not None else os.environ
        configured = configured or env.get(
            "MACAST_BLUETOOTH_AUDIO_EXE", "")
        if configured:
            return BluetoothAudioProcessManager._resolve_path(configured)

        search_dirs = list(search_dirs or [])
        search_dirs.extend([
            os.path.dirname(os.path.abspath(sys.argv[0])),
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        ])
        if getattr(sys, "frozen", False):
            search_dirs.append(
                os.path.dirname(os.path.abspath(sys.executable)))

        names = [
            "macast-bluetooth-audio.exe",
            "AudioPlaybackConnector64.exe",
        ]
        subdirs = ["", "bin", os.path.join("bin", "BluetoothAudio")]
        for directory in search_dirs:
            if not directory:
                continue
            for subdir in subdirs:
                for name in names:
                    resolved = BluetoothAudioProcessManager._resolve_path(
                        os.path.join(directory, subdir, name))
                    if resolved:
                        return resolved
        return None

    def start(self):
        executable = self.discover_executable(
            self._configured_executable, env=self._env)
        with self._lock:
            if self._process is not None and self._process.poll() is None:
                return True
            if not executable:
                self._set_state(
                    self.MISSING,
                    "Bluetooth audio backend was not found")
                return False
            if self.working_dir:
                os.makedirs(self.working_dir, exist_ok=True)
            startupinfo = None
            if sys.platform == "win32":
                startupinfo = subprocess.STARTUPINFO()
                startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
                startupinfo.wShowWindow = 0
            command = [
                executable,
                "--pipe",
                self.PIPE_NAME,
                "--state",
                self.state_path,
                "--log",
                self.log_path,
            ]
            try:
                self.executable = executable
                self._process = self._popen_factory(
                    command,
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    env=self._env,
                    cwd=self.working_dir,
                    startupinfo=startupinfo)
            except Exception as error:
                logger.exception("Unable to start Bluetooth audio backend")
                self._process = None
                self._set_state(self.ERROR, str(error))
                return False
            self._set_state(self.STARTING, "Starting Bluetooth audio")
            self._stop_event.clear()

        deadline = time.monotonic() + self.startup_timeout
        while time.monotonic() < deadline:
            with self._lock:
                process = self._process
            if process is None or process.poll() is not None:
                self._set_state(
                    self.ERROR,
                    "Bluetooth audio backend exited during startup")
                return False
            if self.send_command("PING")[0]:
                self.send_command("AUTO ON")
                self._set_state(self.READY, "Bluetooth audio receiver is ready")
                self.refresh_devices()
                self._ensure_monitor()
                return True
            time.sleep(0.1)
        self.stop()
        self._set_state(self.ERROR, "Bluetooth audio backend did not start")
        return False

    def stop(self):
        with self._lock:
            process = self._process
            monitor = self._monitor
            self._monitor = None
            self._stop_event.set()
        if process is not None and process.poll() is None:
            try:
                self.send_command("SHUTDOWN", timeout_ms=500)
                process.wait(timeout=self.shutdown_timeout)
            except Exception:
                try:
                    process.terminate()
                    process.wait(timeout=1.0)
                except Exception:
                    try:
                        process.kill()
                        process.wait(timeout=1.0)
                    except Exception:
                        logger.exception(
                            "Unable to stop Bluetooth audio backend")
        if monitor is not None and monitor is not threading.current_thread():
            monitor.join(timeout=self.poll_interval * 2)
        with self._lock:
            self._process = None
            self._device_id = ""
            self._device_name = ""
            self._error = ""
            self._last_state_signature = None
        self._set_state(self.STOPPED, "Bluetooth audio receiver is stopped")
        return True

    def restart(self):
        self.stop()
        return self.start()

    def refresh_devices(self):
        ok, response = self.send_command("LIST")
        if not ok:
            return []
        try:
            payload = json.loads(response)
            devices = []
            for device in payload.get("devices", []):
                devices.append({
                    "id": bytes.fromhex(
                        device.get("id_hex", "")).decode(
                            "utf-8", errors="replace"),
                    "name": bytes.fromhex(
                        device.get("name_hex", "")).decode(
                            "utf-8", errors="replace"),
                    "connected": bool(device.get("connected", False)),
                })
            devices.sort(key=lambda item: item["name"].casefold())
        except (ValueError, TypeError, json.JSONDecodeError):
            logger.exception("Unable to decode Bluetooth device list")
            return []
        with self._lock:
            self._devices = devices
        return list(devices)

    def connect_device(self, device_id, device_name=""):
        encoded = device_id.encode("utf-8").hex().upper()
        ok, response = self.send_command(
            "CONNECT {}".format(encoded), timeout_ms=20000)
        if ok:
            with self._lock:
                self._device_id = device_id
                self._device_name = device_name
            self._set_state(
                self.CONNECTED,
                "Bluetooth audio connected{}".format(
                    ": " + device_name if device_name else ""))
        else:
            self._set_state(
                self.ERROR,
                response or "Bluetooth connection failed")
        return ok

    def disconnect_device(self):
        if not self.is_running():
            return False
        self.send_command("AUTO OFF")
        ok, response = self.send_command("DISCONNECT")
        if ok:
            with self._lock:
                self._device_id = ""
                self._device_name = ""
            self._set_state(
                self.READY, "Bluetooth audio receiver is ready")
        return ok

    def suspend_auto(self):
        if not self.is_running():
            return False
        ok, _ = self.send_command("AUTO OFF")
        if ok:
            with self._lock:
                self._device_id = ""
                self._device_name = ""
            self._set_state(
                self.READY, "Bluetooth audio is waiting for preemption")
        return ok

    def resume_auto(self):
        if not self.is_running():
            return False
        return self.send_command("AUTO ON")[0]

    def send_command(self, command, timeout_ms=3000):
        if sys.platform != "win32":
            return False, "Bluetooth audio is Windows-only"
        import ctypes
        from ctypes import wintypes

        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.WaitNamedPipeW.argtypes = [
            wintypes.LPCWSTR, wintypes.DWORD]
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

        if not kernel32.WaitNamedPipeW(self.PIPE_NAME, timeout_ms):
            return False, "Bluetooth audio backend is not ready"
        handle = kernel32.CreateFileW(
            self.PIPE_NAME, 0x80000000 | 0x40000000, 0,
            None, 3, 0, None)
        if handle == wintypes.HANDLE(-1).value:
            return False, "Unable to open Bluetooth audio control pipe"
        try:
            payload = (command + "\n").encode("ascii")
            written = wintypes.DWORD()
            if not kernel32.WriteFile(
                    handle, payload, len(payload),
                    ctypes.byref(written), None):
                return False, "Unable to send Bluetooth audio command"
            buffer = ctypes.create_string_buffer(65536)
            read = wintypes.DWORD()
            if not kernel32.ReadFile(
                    handle, buffer, len(buffer),
                    ctypes.byref(read), None):
                return False, "Unable to read Bluetooth audio response"
            reply = buffer.raw[:read.value].decode(
                "utf-8", errors="replace").strip()
        finally:
            kernel32.CloseHandle(handle)
        if reply.startswith("OK"):
            return True, reply[2:].strip()
        return False, reply[3:].strip() if reply.startswith("ERR") else reply

    def _set_state(self, state, message):
        with self._lock:
            self._state = state
            self._message = message
            callback = self.state_callback
            snapshot = self._snapshot_locked()
        if callback is not None:
            try:
                callback(snapshot)
            except Exception:
                logger.exception("Bluetooth audio callback failed")

    def _snapshot_locked(self):
        return {
            "state": self._state,
            "message": self._message,
            "device_id": self._device_id,
            "device_name": self._device_name,
            "error": self._error,
        }

    def _ensure_monitor(self):
        with self._lock:
            if self._monitor is not None and self._monitor.is_alive():
                return
            self._monitor = threading.Thread(
                target=self._monitor_state,
                name="BLUETOOTH_AUDIO_MONITOR",
                daemon=True)
            self._monitor.start()

    def _monitor_state(self):
        while not self._stop_event.wait(self.poll_interval):
            if not self.is_running():
                self._set_state(
                    self.ERROR, "Bluetooth audio backend exited")
                return
            values = self._read_state_file()
            if values is None:
                continue
            signature = (
                values["state"], values["device_id"], values["device_name"],
                values["error"])
            with self._lock:
                if signature == self._last_state_signature:
                    continue
                self._last_state_signature = signature
                raw_state = values["state"]
                self._state = {
                    "disconnected": self.READY,
                    "connecting": self.CONNECTING,
                    "connected": self.CONNECTED,
                    "error": self.ERROR,
                }.get(raw_state, raw_state)
                self._device_id = values["device_id"]
                self._device_name = values["device_name"]
                self._error = values["error"]
                self._message = (
                    values["error"] or
                    values["device_name"] or
                    self._state)
                callback = self.state_callback
                snapshot = self._snapshot_locked()
            if callback is not None:
                try:
                    callback(snapshot)
                except Exception:
                    logger.exception("Bluetooth audio callback failed")

    def _read_state_file(self):
        path = self.state_path
        if not path or not os.path.isfile(path):
            return None
        values = {}
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as stream:
                for line in stream:
                    key, separator, value = line.rstrip().partition("=")
                    if separator:
                        values[key] = value
            decoded = {"state": values.get("state", "")}
            for key in ("device_id", "device_name", "error"):
                encoded = values.get(key + "_hex", "")
                decoded[key] = bytes.fromhex(encoded).decode(
                    "utf-8", errors="replace") if encoded else ""
            return decoded
        except (OSError, ValueError):
            return None
