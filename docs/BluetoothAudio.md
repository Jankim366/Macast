# Bluetooth Audio Receiver

Macast can manage a hidden Windows Bluetooth A2DP sink backend. This lets a
paired phone send audio to the PC while Macast remains the only management
interface.

## Requirements

- Windows 10 version 2004 (build 19041) or newer.
- Windows 11 is recommended.
- A Bluetooth adapter and driver that support the A2DP sink role.
- The phone must first be paired in **Windows Settings > Bluetooth & devices**.

Pairing is intentionally left to Windows. The app cannot replace the system
pairing and driver compatibility layer.

## Usage

1. Pair the phone in Windows Settings.
2. Start Macast. The hidden Bluetooth backend starts automatically.
3. Any compatible paired Bluetooth audio device reported by Windows is
   connected automatically. No device selection is required in Macast.
4. Play audio on the phone. The newest playing source preempts AirPlay or DLNA.
5. Use the Macast Bluetooth submenu only for status, temporary stop/restart,
   or opening Windows Bluetooth settings.

The backend uses a `DeviceWatcher`, so devices added after Macast starts are
also picked up without restarting the application.

## Arbitration

Bluetooth audio participates in the same last-sender-wins arbitration as
AirPlay and DLNA:

- A new Bluetooth connection pauses controllable AirPlay audio and stops DLNA.
- A new AirPlay session disconnects Bluetooth audio and stops DLNA.
- A new DLNA item disconnects Bluetooth audio and suspends AirPlay audio.
- Multiple audio sources are never intentionally mixed.

## Limitations

- A2DP is audio-only. It does not provide screen mirroring.
- The Windows `AudioPlaybackConnection` API does not expose title, artist,
  cover art, playlists, or transport commands to the receiver.
- Codec selection (SBC, AAC, aptX, LDAC, and so on) is controlled by Windows
  and the Bluetooth driver.
- Audio latency is higher than AirPlay or DLNA and may be unsuitable for
  synchronized video.
- The backend performs one internal reconnect on its first activation to
  encourage Windows 11 to create the A2DP output endpoint. Some adapters may
  still require a manual disconnect/reconnect, as documented by the upstream
  AudioPlaybackConnector project.

## Source and license

The headless backend uses the Windows `AudioPlaybackConnection` lifecycle
approach from
[summerwind6/AudioPlaybackConnector-Win11](https://github.com/summerwind6/AudioPlaybackConnector-Win11),
which is based on Richard Yu's AudioPlaybackConnector. That project is
available under the MIT License. Its notice is included with the packaged
application under `licenses/LICENSE.audioplaybackconnector`.

Build the backend with:

```powershell
powershell -ExecutionPolicy Bypass -File tools/build_bluetooth_audio_backend.ps1
```

