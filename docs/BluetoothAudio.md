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
2. Open `Macast > Setting > Bluetooth Audio Receiver`.
3. Click `Start Bluetooth Audio`, then select the paired phone.
4. Play audio on the phone. Windows routes it to the current audio output.
5. Select the connected phone again, or click `Stop Bluetooth Audio`, to
   disconnect it.

Macast stores the selected device and can reconnect it automatically after a
later launch.

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
- Some Bluetooth devices report a connected state but need one disconnect and
  reconnect before audio starts. This behavior also exists in the upstream
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

