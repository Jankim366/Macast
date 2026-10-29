# AirPlay Screen Mirroring

This fork manages [AirPlay-Windows](https://github.com/moieric11/AirPlay-Windows)
as a separate AirPlay receiver process. AirPlay-Windows provides H.264/HEVC
screen mirroring with RAOP audio on Windows 10 and 11.

The integration is Windows-only. UxPlay remains the upstream protocol
reference, but Macast does not launch or package its MSYS2/GStreamer build.

## Runtime behavior

- Macast starts AirPlay-Windows when `AirPlayEnabled` is enabled and the
  executable can be found.
- Autostart is scheduled just after tray and service bootstrap. If a previous
  receiver is still releasing `Local\AirPlay-Windows.Instance.v1`, Macast
  waits for it before launching the replacement.
- The menu remains in `Starting` until both `_airplay` and `_raop` mDNS
  registrations report success. A receiver that exits or fails to publish its
  services during this startup window is retried automatically instead of
  leaving AirPlay stopped until a manual restart.
- Windows mDNS registration is bound to the selected physical LAN interface,
  including its concrete IPv4 address and interface index. Proxy, VPN,
  container and other virtual adapters are skipped, so a FlClash/ZeroTier
  route cannot publish AirPlay only on `198.18.0.1` or `10.147.x.x`.
- `deviceid` and `pi` are deterministic values derived from the persistent
  Ed25519 key rather than process-wide constants or a fallback `02:00:...`
  MAC. The same identity is reused on every launch, while separate Macast
  installations receive distinct receiver IDs.
- The `_airplay` instance label includes a short identity suffix and the
  `_raop` instance name uses the full derived device ID. Same-named Macast
  receivers on one LAN therefore remain separate DNS-SD services and can all
  appear in the iOS AirPlay picker. The SRV target deliberately remains the
  real Windows host name, because inventing a host label can make the service
  discoverable while resolving to a VPN/proxy address.
- The LAN regression test resolves the registered service through Windows
  DNS-SD and requires the returned IPv4 address to match the physical interface.
  A browse-only test is not sufficient for AirPlay connection readiness.
- The receiver checks the selected LAN interface and IPv4 once per second.
  When Windows changes networks, it stops the old mDNS registration and
  advertises the same receiver on the new physical LAN without restarting the
  AirPlay process.
- DACP and mDNS both use the same physical LAN interface index. The LAN tests
  register a temporary receiver and verify that `_airplay._tcp.local` is
  immediately discoverable on that interface before packaging.
- When the AirPlay control session uses a link-local IPv6 peer address, Macast
  retains the interface scope (for example `fe80::...%26`) when connecting to
  the resolved DACP port. Without the scope, Windows cannot reach the sender
  even though mDNS resolution itself succeeds.
- The one-file Macast build extracts to a different temporary directory each
  launch. Macast therefore copies the self-contained AirPlay executable to a
  stable path under `SETTING_DIR\AirPlay-Windows` and stores its source hash.
  Firewall rules are keyed to that stable executable path instead of the
  transient `_MEI...` copy.
- On first use of the stable backend path, Macast invokes the receiver's
  `--configure-firewall` helper. It may raise one UAC prompt. The helper adds a
  LocalSubnet inbound allow rule and disables stale per-executable Block rules;
  later backend updates at that same path do not prompt again.
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
- AirPlay audio metadata is forwarded to Macast through a local named pipe and
  state file: title, artist, album, elapsed/total time, playback rate, cover
  availability, and DACP control availability.
- The original Macast AirPlay submenu exposes Previous, Play, Pause,
  Toggle Play/Pause, Next, Volume Up, Volume Down, and Toggle Mute.
- Latest sender wins for simultaneous connections. A new AirPlay session
  disconnects the older AirPlay sender and stops active DLNA playback. A new
  DLNA URI suspends or disconnects active AirPlay and replaces the current mpv
  item. The receiver is not rejected and the two protocols are never mixed.
- When DLNA interrupts controllable AirPlay audio, Macast pauses that iPhone
  session through DACP instead of tearing down the connection. After the
  DLNA item stops, AirPlay playback resumes automatically. AirPlay mirroring
  and senders without working DACP controls still use hard preemption.
- Macast sends reverse controls with the sender-provided `DACP-ID` and
  `Active-Remote` token. Commands are only enabled when the sender exposes a
  usable DACP service. DACP discovery uses the native Windows service browser
  (`_dacp._tcp.local`) instead of relying only on a one-shot service-name
  resolve, so later service registration and port changes are picked up.
- Windows SMTC is published from a dedicated STA thread. The card uses the
  explicit `Jankim366.Macast.AirPlay` AppUserModelID, shows the current title,
  artist and album, and exposes previous, play/pause and next controls only
  when DACP is reachable.
- On Windows, the bundled mpv publishes its own SMTC card while DLNA media is
  playing. The card exposes the current title and Play/Pause control. mpv is
  idle when Macast is not casting, so it does not leave a stale DLNA card next
  to the AirPlay card. Global media-key routing stays with Windows and other
  applications.
- The Windows package uses `mpv-x86_64-20261021-git-e471496c56.7z`
  (`mpv v0.41.0-1227-ge471496c5`). mpv 0.34 does not contain the Windows SMTC
  implementation required for the DLNA media card.
- The audio sink logs the SDL driver and selected endpoint. Remote-desktop and
  obviously virtual output endpoints are skipped when a local playback device
  is available. Set `MACAST_AIRPLAY_AUDIO_DEVICE` to the exact endpoint name
  when a machine needs an explicit override.
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
  one-file builds. The public AirPlay `deviceid`, `pi`, and discovery suffixes
  are all derived from this key and update together if the identity is reset.

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
the reproducibility patches under `tools/`:

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
2. Build the clean AirPlay backend with
   `tools/build_airplay_windows_macast.ps1`.
3. In that build tree, configure `-DBUILD_NETWORK_TESTS=ON` and run
   `test-shutdown --dacp`, `test-audio-timing`, `test-audio-output` and
   `test-media-session`.
4. Start Macast and open `Setting -> AirPlay Screen Mirroring`.
5. Confirm the menu changes from `Starting` to `Running`.
6. On an iPhone or iPad, open Screen Mirroring and select the configured
   AirPlay-Windows receiver.
7. Confirm mirrored video and audible output on the endpoint printed in
   `airplay-windows.log`.
8. Play audio in Apple Music and confirm the Macast submenu and Windows media
   card show the same title. Verify previous, pause/resume and next against
   the phone; confirm `control=1` in
   `macast_airplay_state.ini`.
9. Use `Restart AirPlay` and confirm the sender can reconnect.
10. Quit Macast and confirm `airplay-windows.exe` no longer exists in Task
   Manager.

## Known limits and distribution risk

- AirPlay-Windows supports one active sender session at a time.
- DRM-protected video is not supported.
- Experimental HLS direct playback is not enabled by Macast by default.
- DACP metadata/control support depends on the sender app. Lyrics, the complete
  playlist, and next-track preview are not normally exposed.
- Seeking is implemented but may be rejected by some iOS sender versions.
- AirPlay-Windows is a new project. Macast pins v0.4.1 instead of following its
  latest release automatically.
- AirPlay-Windows is GPL-3.0, but its third-party notices state that the bundled
  FairPlay reply bytes originated from Apple and that the project makes no
  ownership or license claim over those bytes. Review that distribution risk
  before publishing a public installer.
