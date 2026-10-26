<img align="center" src="macast_slogan.png" alt="slogan" height="auto"/>

# Macast

[![visitor](https://visitor-badge.glitch.me/badge?page_id=xfangfang.Macast)](https://github.com/xfangfang/Macast/releases/latest)
![stars](https://img.shields.io/badge/dynamic/json?label=github%20stars&query=stargazers_count&url=https%3A%2F%2Fapi.github.com%2Frepos%2Fxfangfang%2FMacast)
[![downloads](https://img.shields.io/github/downloads/xfangfang/Macast/total?color=blue)](https://github.com/xfangfang/Macast/releases/latest)
[![plugins](https://shields-staging.herokuapp.com/github/directory-file-count/xfangfang/Macast-plugins?type=dir&label=plugins)](https://github.com/xfangfang/Macast-plugins)
[![pypi](https://img.shields.io/pypi/v/macast)](https://pypi.org/project/macast/)
[![aur](https://img.shields.io/aur/version/macast-git?color=yellowgreen)](https://aur.archlinux.org/packages/macast-git/)
[![build](https://img.shields.io/github/workflow/status/xfangfang/Macast/Build%20Macast)](https://github.com/xfangfang/Macast/actions/workflows/build-macast.yaml)
[![mac](https://img.shields.io/badge/MacOS-10.14%20and%20higher-lightgrey?logo=Apple)](https://github.com/xfangfang/Macast/releases/latest)
[![windows](https://img.shields.io/badge/Windows-7%20and%20higher-lightgrey?logo=Windows)](https://github.com/xfangfang/Macast/releases/latest)
[![linux](https://img.shields.io/badge/Linux-Xorg-lightgrey?logo=Linux)](https://github.com/xfangfang/Macast/releases/latest)



[中文说明](README_ZH.md)

A menu bar application using mpv as **DLNA Media Renderer**. You can push videos,
pictures or music from your mobile phone to your computer.

This fork additionally supports **AirPlay screen mirroring and audio on
Windows** while keeping the original Macast menu and settings interface.


## Installation

- ### MacOS || Windows || Debian

  Download link:  [Macast release latest](https://github.com/xfangfang/Macast/releases/latest)

- ### Package manager

  ```shell
  pip install macast
  macast-gui # or macast-cli
  ```

  Please see our wiki for more information(like **aur** support): [#package-manager](https://github.com/xfangfang/Macast/wiki/Installation#package-manager)  
  Linux users may have problems installing using pip. Two additional libraries that I have modified need to be installed:

  ```shell
  pip install git+https://github.com/xfangfang/pystray.git
  pip install git+https://github.com/xfangfang/pyperclip.git
  ```

  **See [this](https://github.com/xfangfang/Macast/wiki/Installation#linux) for Linux compatibility**

- ### Build from source

  Please refer to: [Macast Development](docs/Development.md)


## Usage

- **For ordinary users**  
After opening this app, a small icon will appear in the **menubar** / **taskbar** / **desktop panel**, then you can push your media files from a local DLNA client to your computer.

- **For advanced users**  
  1. By loading the [Macast-plugins](https://github.com/xfangfang/Macast-plugins), Macast can support third-party players like IINA and PotPlayer.  
  For more information, see: [#how-to-use-third-party-player-plug-in](https://github.com/xfangfang/Macast/wiki/FAQ#how-to-use-third-party-player-plug-in)
  2. You can modify the shortcut keys or configuration of the default mpv player by yourself, see: [#how-to-set-personal-configurations-to-mpv](https://github.com/xfangfang/Macast/wiki/FAQ#how-to-set-personal-configurations-to-mpv)
  3. This fork can manage an AirPlay-Windows receiver process for AirPlay screen
  mirroring and audio on Windows.

- **For developer**  
You can use a few lines of code to add support for other players like IINA and PotPlayer or even add additional features, like downloading media files while playing videos.  
Tutorials and examples are shown in: [Macast/wiki/Custom-Renderer](https://github.com/xfangfang/Macast/wiki/Custom-Renderer).  
Fell free to submit a pull request to [Macast-plugins](https://github.com/xfangfang/Macast-plugins).  

## Windows AirPlay Mirroring

This fork extends Macast with an independent
[AirPlay-Windows](https://github.com/moieric11/AirPlay-Windows) receiver:

- iPhone/iPad screen mirroring with H.264 or HEVC and AirPlay audio.
- DLNA and AirPlay can run at the same time.
- The original Macast tray/menu and web settings remain the control interface.
- AirPlay-Windows starts hidden, so it does not open a second management UI.
- AirPlay autostart waits for a previous instance to release its mutex and
  reports `Running` only after both `_airplay` and `_raop` mDNS registrations
  succeed. Early exits and failed registrations are retried automatically.
- AirPlay publishing is pinned to the physical Wi-Fi/Ethernet interface and its
  real IPv4/MAC address. FlClash, ZeroTier and VPN adapters are not allowed to
  capture the AirPlay mDNS record; DACP browsing uses the same LAN interface.
- The backend watches the physical LAN address once per second and automatically
  withdraws/re-publishes mDNS after switching Wi-Fi, a phone hotspot or Ethernet.
- The packed backend is staged in Macast's stable user-data directory so its
  Windows Firewall rule survives PyInstaller's changing `_MEI...` directories.
  The first run requests administrator permission once to disable stale
  per-executable Block rules and create a LocalSubnet inbound Allow rule.
- During mirroring, a standard Windows video window is shown with native
  minimize, maximize, and close controls.
- The video window follows the current portrait or landscape aspect ratio.
  Edge dragging is ratio-constrained and remains usable for resizing.
- The AirPlay receiver name follows Macast's normal friendly-name setting.
- Macast's native AirPlay submenu shows the current title/artist/time and
  provides Previous, Play, Pause, Toggle Play/Pause, Next, Volume Up, Volume
  Down, and Toggle Mute.
- Windows also shows a `Macast AirPlay` system media card with the current
  title, artist, timeline, and working Previous/Pause-or-Play/Next controls.
- Windows DLNA playback publishes an mpv system media card with the current
  media title and working Play/Pause control. The card is removed when DLNA
  playback becomes idle.
- The latest sender wins. A new AirPlay device disconnects the previous
  AirPlay session and stops DLNA; a new DLNA cast disconnects AirPlay and
  replaces the current item. Multiple senders are never mixed.
- DACP services are discovered through the native Windows `_dacp._tcp.local`
  browser, including services registered after the AirPlay connection.
- Audio logs its actual SDL/WASAPI endpoint. Remote-desktop and virtual
  endpoints are avoided when a local speaker is available, and
  `MACAST_AIRPLAY_AUDIO_DEVICE` can select an exact output device.
- AirPlay metadata and reverse controls use the sender's DMAP/DACP services;
  lyrics, complete playlists, and next-track previews are normally unavailable.
- Macast stops the AirPlay process when it exits. Windows Job Objects are used
  to avoid orphaned receiver processes after an abnormal Macast exit.

The first launch may require allowing `airplay-windows-macast.exe` through
Windows Firewall for the local network. See [docs/AirPlay.md](docs/AirPlay.md)
for first-run setup, configuration, known limits, and packaging details.

### Build the Windows AirPlay backend

The AirPlay backend is based on a pinned AirPlay-Windows source release and a
small Macast compatibility patch:

```powershell
powershell -ExecutionPolicy Bypass -File tools/prepare_airplay_windows.ps1
powershell -ExecutionPolicy Bypass -File tools/build_airplay_windows_macast.ps1
```

The first backend build installs FFmpeg, OpenSSL, and SDL2 through vcpkg and can
take several minutes. It produces a self-contained
`bin\AirPlay-Windows\airplay-windows-macast.exe`.

Known limits: one active AirPlay sender at a time, no DRM-protected video
playback, and Windows 10/11 x64 is the supported AirPlay host platform.


## FAQ
If you have any questions about this application, please check: [Macast/wiki/FAQ](https://github.com/xfangfang/Macast/wiki/FAQ).  
If this does not solve your problem, please open a new issue to notify us, we are willing to help you solve the problem.

## Screenshots

You can copy the video link after the video is casted：  
<img align="center" width="400" src="https://github.com/xfangfang/xfangfang.github.io/raw/master/assets/img/macast/copy_uri.png" alt="copy_uri" height="auto"/>

Or select a third-party player plug-in  
<img align="center" width="400" src="https://github.com/xfangfang/xfangfang.github.io/raw/master/assets/img/macast/select_renderer.png" alt="select_renderer" height="auto"/>

## Relevant links

[UPnP™ Device Architecture 1.1](http://upnp.org/specs/arch/UPnP-arch-DeviceArchitecture-v1.1.pdf)

[UPnP™ Resources](http://upnp.org/resources/upnpresources.zip)

[UPnP™ ContentDirectory:1 service](http://upnp.org/specs/av/UPnP-av-ContentDirectory-v1-Service.pdf)

[UPnP™ MediaRenderer:1 device](http://upnp.org/specs/av/UPnP-av-MediaRenderer-v1-Device.pdf)

[UPnP™ AVTransport:1 service](http://upnp.org/specs/av/UPnP-av-AVTransport-v1-Service.pdf)

[UPnP™ RenderingControl:1 service](http://upnp.org/specs/av/UPnP-av-RenderingControl-v1-Service.pdf)

[python-upnp-ssdp-example](https://github.com/ZeWaren/python-upnp-ssdp-example)
