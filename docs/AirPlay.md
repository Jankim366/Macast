# AirPlay Screen Mirroring

This fork manages [AirPlay-Windows](https://github.com/moieric11/AirPlay-Windows)
as a separate AirPlay receiver process. AirPlay-Windows provides H.264/HEVC
screen mirroring with RAOP audio on Windows 10 and 11.

The integration is Windows-only. UxPlay remains the upstream protocol
reference, but Macast does not launch or package its MSYS2/GStreamer build.

## Runtime behavior

- Macast starts AirPlay-Windows when `AirPlayEnabled` is enabled and the
  executable can be found.
- `Setting -> AirPlay Screen Mirroring` displays the process state and provides
  start, stop, restart, and show-window actions.
- AirPlay-Windows starts hidden. Its standalone control interface does not open
  when Macast starts.
- Macast uses the locally built `airplay-windows-macast.exe`, which creates a
  standard resizable Windows frame. The minimize, maximize, and close buttons
  are provided by Windows, not by a copied AirPlay-Windows UI.
- When the AirPlay-Windows log reports a screen-mirroring connection, Macast
  shows the receiver window in chromeless mode so it contains only mirrored
  video. The window is hidden again two seconds after the mirror stream ends.
- The visible window is fitted to the decoded video resolution, so portrait
  and landscape mirrors keep their native aspect ratio without side or top
  black bars.
- While the window is visible, Macast checks the client-area ratio every 30 ms.
  Manual resizing is constrained to the current video ratio; maximizing fits
  the largest aspect-correct window on the active monitor.
- Resize direction follows the edge being dragged. Dragging the top or bottom
  keeps the new height and changes width; dragging a side keeps the new width
  and changes height. The opposite edge stays anchored.
- `Show Mirror Window` is only for explicit inspection or first-time firewall
  setup. It does not become the primary Macast interface.
- The AirPlay receiver name is written from Macast's original
  `Setting.get_friendly_name()` value. iOS and macOS therefore advertise the
  same `Macast(...)` name used by Macast's DLNA service instead of
  `AirPlay-Windows`.
- `Start Cast` controls the DLNA service. It does not stop the independent
  AirPlay receiver.
- A normal AirPlay-Windows exit is not restarted. A non-zero exit is retried at
  most three times per stable run.
- Closing Macast requests a graceful SDL window close. A Windows Job Object also
  terminates the receiver if Macast exits unexpectedly.
- Receiver output is appended to `airplay-windows.log` in Macast's user
  configuration directory.
- `identity.key` is stored in `SETTING_DIR\AirPlay-Windows` so the sender can
  keep recognizing the receiver across launches, including PyInstaller
  one-file builds.

Macast does not embed the mirrored video into its own menu or browser UI.
AirPlay-Windows creates its own SDL window; Macast manages and reveals it.
No AirPlay-Windows interface, layout, icons, or settings page is copied into
Macast. The Macast tray/menu and web settings page keep their original design.

## Finding AirPlay-Windows

Macast checks:

1. `AirPlayExecutable` in `macast_setting.json`, which must point to the custom
   build when explicitly set.
2. `MACAST_AIRPLAY_EXE`.
3. `bin\AirPlay-Windows\airplay-windows-macast.exe` next to Macast.
4. `bin\airplay-windows.exe` and other adjacent development locations.

The recommended source-checkout and packaged location is:

```text
Macast/
  bin/
    AirPlay-Windows/
      airplay-windows-macast.exe
      SDL2.dll
      avcodec-62.dll
      avformat-62.dll
      avutil-60.dll
      ...
```

The complete official release directory must be kept together. Copying only
the EXE will fail because AirPlay-Windows requires the adjacent DLLs.

Prepare the pinned release used by Macast:

```powershell
powershell -ExecutionPolicy Bypass -File tools/prepare_airplay_windows.ps1
```

The script downloads AirPlay-Windows v0.4.1, verifies its SHA-256, and stages the
complete runtime under `bin\AirPlay-Windows`.

Build the Macast-compatible standard-frame binary from the pinned source and
the patch in `tools/airplay-windows-macast.patch`:

```powershell
powershell -ExecutionPolicy Bypass -File tools/build_airplay_windows_macast.ps1
```

The first build installs FFmpeg/OpenSSL/SDL2 through the pinned vcpkg baseline
and can take several minutes. The result is a self-contained 64-bit
`airplay-windows-macast.exe`; the official multi-file release remains available
as a fallback only.

For an installation elsewhere:

```powershell
$env:MACAST_AIRPLAY_EXE = 'D:\Tools\AirPlay-Windows\airplay-windows.exe'
python Macast.py
```

## Settings

The settings are stored in Macast's normal `macast_setting.json`. Edit them
while Macast is closed.

```json
{
  "AirPlayEnabled": 1,
  "AirPlayExecutable": "",
  "AirPlayAutoRestart": 1,
  "AirPlayExtraArguments": [],
  "AirPlayHideWindowsUI": 1
}
```

- `AirPlayEnabled`: start AirPlay-Windows with Macast. Menu actions update it.
- `AirPlayExecutable`: optional explicit EXE path. An invalid explicit path is
  reported instead of silently choosing another executable.
- `AirPlayAutoRestart`: retry abnormal exits.
- `AirPlayExtraArguments`: additional AirPlay-Windows arguments. Macast always
  adds `--log` so output is captured.
- `AirPlayHideWindowsUI`: keep AirPlay-Windows hidden until mirroring starts and
  enable its chromeless video mode. Set this to `0` only when configuring the
  standalone AirPlay-Windows interface is necessary.

AirPlay-Windows stores its own settings in
`%APPDATA%\AirPlay-Windows\settings.ini`. Set firewall permission, mirror
decoder, resolution, and rotation through its standalone window when needed.
Macast manages `receiver_name`, `chromeless`, and its own persistent identity;
the remaining display preferences still use this file.

Recommended option examples:

```json
"AirPlayExtraArguments": ["--mirror-hwaccel", "--mirror-res", "1920x1080"]
```

Omit `--mirror-res` to use AirPlay-Windows' detected display or saved setting.

## Firewall and first launch

The first launch may require allowing `airplay-windows.exe` through Windows
Firewall on the local network. For first-time setup only, set
`AirPlayHideWindowsUI` to `0`, restart Macast, and use AirPlay-Windows' `Allow
on local network` action. Restore it to `1` afterwards. Do not disable the
firewall for all networks.

If AirPlay-Windows was already started outside Macast, Macast reports it as an
external instance instead of launching a second process. Close the external
instance before starting the Macast-managed receiver.

## Packaging

`tools/prepare_airplay_windows.ps1` pins AirPlay-Windows v0.4.1 and verifies:

```text
SHA256 5f1792a96efffe84b220edfa7286fa7670806226aa4d0b4b6c7e33ddd7747bf9
```

The Windows PyInstaller build adds the entire directory:

```text
--add-data="bin/AirPlay-Windows;bin/AirPlay-Windows"
```

The complete directory is required; `--add-binary` for the EXE alone is not
sufficient because AirPlay-Windows also loads SDL2 and FFmpeg DLLs.

## Verification checklist

1. Run `python -m unittest discover -s tests -v`.
2. Start Macast and open `Setting -> AirPlay Screen Mirroring`.
3. Confirm the menu changes from `Starting` to `Running`.
4. On an iPhone or iPad, open Screen Mirroring and select the configured
   AirPlay-Windows receiver.
5. Confirm mirrored video and its audio.
6. Use `Restart AirPlay` and confirm the sender can reconnect.
7. Quit Macast and confirm `airplay-windows.exe` no longer exists in Task
   Manager.

## Known limits and distribution risk

- AirPlay-Windows supports one active sender session at a time.
- DRM-protected video is not supported.
- Experimental HLS direct playback is not enabled by Macast by default.
- AirPlay-Windows is a new project. Macast pins v0.4.1 instead of following its
  latest release automatically.
- AirPlay-Windows is GPL-3.0, but its third-party notices state that the bundled
  FairPlay reply bytes originated from Apple and that the project makes no
  ownership or license claim over those bytes. Review that distribution risk
  before publishing a public installer.
