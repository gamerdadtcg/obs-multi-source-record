# Multi Source Record (MSR)

**Beta 0.3.0** — this is a public beta. Features and UI can still change. Please report bugs; do not treat this as a finished release.

MSR is an [OBS Studio](https://obsproject.com/) plugin for **per-source ISO recording**. Pick the sources you want, hit one Start button, and each selected source is recorded to its own file. It does **not** change OBS mixer track assignments, and it does **not** touch OBS’s main stream or main recording.

## This is a beta

- Windows installer only for now (build tag `58d5896`)
- Tested with OBS Studio **30+** (built against OBS 31.1.1)
- If something breaks, email **[bugs@kickinit.app](mailto:bugs@kickinit.app)** or open a [GitHub Issue](https://github.com/gamerdadtcg/obs-multi-source-record/issues)

## What it does

- **Per-source ISO recording** — one file per selected source
- **Per-source audio** — each row defaults to that source’s own audio, or you can pick a different audio source (for example, a mic on a silent display capture). Only the selected audio is recorded. MSR never remaps OBS mixer tracks.
- **Output-driven folder and filenames** — the Output section sets the folder, container, and filename pattern, including the **date and 12-hour time**
- **Recording timer** in the dock while a recording is running
- **Last Recorded ISOs** — cards at the bottom of the dock show the full path of the files you just recorded
- **Multi-RTMP** destinations for the main (horizontal) canvas or the **MSR Vertical** canvas, including X vertical
- Optional shared **MSR Vertical** canvas (OBS 31+) with its own preview dock
- Auto-start with OBS recording and/or streaming (off by default)
- Per-source replay buffer (clipping), file split, chapters, hotkeys, and Stream Deck / obs-websocket vendor requests

## Install (Windows)

1. Download the current beta installer: [MSR-Beta-Setup](https://drive.google.com/file/d/1gklpdcJmhCjVZsF4AHLX1XOxMVz5wN2o/view?usp=sharing)
2. Quit OBS.
3. Run the installer.
4. Start OBS and open **View → Docks → Multi Source Record**.

Requirements: **Windows** and **OBS Studio 30 or later**.

You can also build from source for Windows, macOS, or Linux (see below).

## Usage

1. Open the **Multi Source Record** dock.
2. Tick the sources to record.
3. Set each to Horizontal, Vertical, or Both.
4. Leave audio on **Source audio**, or pick a different audio source on that row.
5. Choose the output folder, filename pattern, and container (`mkv` by default, or `mp4`).
6. Click **Start recording**. A timer runs while you are recording.
7. When you stop, the **Last Recorded ISOs** cards show the full file paths.

Vertical (and the vertical half of Both) records the shared **MSR Vertical** canvas once, not a per-source crop. Open **View → Docks → MSR Vertical Preview** to arrange that layout. Switching scenes on main switches the vertical layout with it.

### Hotkeys

Under **Settings → Hotkeys**:

- **Multi Source Record: Start/Stop**
- **Multi Source Record: Pause/Resume**
- **Multi Source Record: Add Chapter**
- **Multi Source Record: Save Replay** (clipping)
- **Multi Source Record: Start/Stop Stream** (MSR destinations only — not OBS’s main stream button)

### Multistream destinations

Add RTMP/RTMPS destinations that publish the **main (horizontal) canvas** or the **MSR Vertical** canvas. These are separate from OBS’s own Start Streaming button and never share OBS stream keys. Stream keys are masked in the dock and never logged.

### Stream Deck and obs-websocket

The repo includes a Stream Deck plugin under `streamdeck/`. You can also call obs-websocket 5 **CallVendorRequest** with vendor name `obs-multi-source-record` for start/stop, pause, chapters, replay, settings, destinations, and Source Record filter control.

### Record All Sources (Source Record filters)

Optional toggle for [Exeldro’s Source Record](https://github.com/exeldro/obs-source-record) filters already on your sources. This does **not** start MSR file outputs or OBS main recording.

## Building from source

This repo uses the [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate) CMake setup.

```bash
cmake --preset windows-x64          # or ubuntu-x86_64 / macos
cmake --build --preset windows-x64
```

## Credits

MSR builds on [Exeldro’s Source Record](https://github.com/exeldro/obs-source-record). Thank you to [@exeldro](https://github.com/exeldro) for that work and for the foundation this plugin stands on.

## License

[GPL-2.0](LICENSE) (required for OBS plugins that link libobs). Compatible with Source Record’s license.

## Bugs and feedback

This is a beta. Please tell us when something goes wrong:

- Email: **[bugs@kickinit.app](mailto:bugs@kickinit.app)**
- GitHub Issues: [gamerdadtcg/obs-multi-source-record/issues](https://github.com/gamerdadtcg/obs-multi-source-record/issues)
