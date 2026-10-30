# Multi Source Record for OBS Studio

A simpler alternative to the classic Source Record filter. Pick the sources you want, choose **Horizontal**, **Vertical** (center-cropped 9:16), or **Both**, and hit one Start button. Each source is written to its **own file** with that source’s **own audio** captured directly (not via mixer tracks 1–6), so sources that only live on tracks 2/3/4 still record with sound.

**Does not touch** OBS’s main stream or main recording.

## Features

- Dock UI checklist of sources
- Per-source orientation: Horizontal / Vertical / Both (two files)
- Direct per-source audio + optional extra audio source (e.g. a mic)
- Output folder + filename pattern `{source}_{orientation}_{date}_{time}`
- Container: `mkv` (default) or `mp4`
- Encoder: Auto (NVENC → AMF → QSV → x264), bitrate in Advanced
- One master Start / Stop
- OBS hotkey: **Multi Source Record: Start/Stop**
- obs-websocket vendor requests for Stream Deck
- Settings saved per OBS profile


## Enable GitHub Actions (one-time)

The first push could not create files under `.github/workflows/` because the
GitHub CLI OAuth token is missing the `workflow` scope. Workflow YAML copies
live in [`build-aux/github-workflows/`](build-aux/github-workflows/).

**Easiest (GitHub website):**

1. Open https://github.com/gamerdadtcg/obs-multi-source-record
2. For each file in `build-aux/github-workflows/`, use **Add file → Create new file**
   with path `.github/workflows/<same-name>` and paste the contents.
   Start with `push.yaml` and `build-project.yaml` (required to build).
3. After `push.yaml` exists on `main`, Actions will build Windows / macOS / Ubuntu.

**Or via CLI on a machine where you can authorize:**

```bash
gh auth refresh -h github.com -s repo,workflow
cd /path/to/obs-multi-source-record
./build-aux/install-github-workflows.sh
git add .github/workflows
git commit -m "Add CI workflows"
git push
```

## Requirements

- OBS Studio 30 or 31 (built against 31.1.1)
- Windows (primary), macOS, or Linux
- obs-websocket (built into OBS 28+)

## Install (Windows)

1. Open the [Actions](../../actions) tab of this repo (or a [Release](../../releases) when tagged).
2. Open the latest successful **Push** workflow run.
3. Download the **windows-x64** artifact zip.
4. Quit OBS.
5. Extract so you have:

   ```
   C:\Program Files\obs-studio\obs-plugins\64bit\obs-multi-source-record.dll
   C:\Program Files\obs-studio\data\obs-plugins\obs-multi-source-record\locale\en-US.ini
   ```

   If you use a portable OBS build, use that folder’s `obs-plugins\64bit` and `data\obs-plugins\` instead.

6. Start OBS. Open **View → Docks → Multi Source Record**.

### Install from a Release tag

Tag a version like `1.0.0` on `main`. The Push workflow creates a **draft** GitHub Release with installers/zips attached. Download the Windows zip or exe from that release.

## Usage

1. Open the **Multi Source Record** dock.
2. Tick the sources to record.
3. Set each to Horizontal, Vertical, or Both.
4. Optionally pick an extra audio source per row (e.g. mic for a silent display capture).
5. Choose the output folder and filename pattern.
6. Click **Save**, then **Start recording**.
7. Files land in the folder as separate `.mkv` (or `.mp4`) files.

### Hotkey

**Settings → Hotkeys → Multi Source Record: Start/Stop**

**Settings → Hotkeys → Multi Source Record: Pause/Resume** — pauses or resumes every active Multi Source Record output together (synced).

**Settings → Hotkeys → Multi Source Record: Add Chapter** — drops a chapter marker on every active output at the same time.

The dock has **Pause** / **Resume** and **Chapter** buttons next to Start/Stop. Pause only applies while recording. Chapter writes a `.chapters.json` sidecar next to each file and calls the output's native `add_chapter` when supported (Hybrid MP4 / OBS 30.2+).

### Vertical crop

Vertical mode center-crops the source to 9:16 and scales to the resolution in Advanced (default **1080×1920**).

## Stream Deck

### Option A — included plugin

1. Install Node.js 18+.
2. Copy `streamdeck/com.travvvman.obs-multi-source-record.sdPlugin` into your Stream Deck plugins folder, e.g.

   `%AppData%\Elgato\StreamDeck\Plugins\`

3. Inside that folder run:

   ```bat
   npm install
   ```

4. Restart Stream Deck.
5. Add **Multi Source Record Toggle**, **Pause**, and/or **Chapter**. Set host/port/password to match **OBS → Tools → WebSocket Server Settings**.
6. Toggle: idle = not recording; red = recording. Pause: recording vs paused. Chapter: marks all active outputs.

### Option B — any websocket tool / Multi Action

Call obs-websocket 5 **CallVendorRequest**:

| Field | Value |
| --- | --- |
| vendorName | `obs-multi-source-record` |
| requestType | `start` / `stop` / `toggle` / `status` / `pause` / `resume` / `toggle_pause` / `add_chapter` |

`add_chapter` accepts optional `requestData.name`. Without a name, chapters are auto-numbered (`Chapter 1`, …).

Example `status` response data: `{ "success": true, "recording": true, "paused": false, "outputs": [...] }`.

## Building from source

This repo uses the [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate) CMake setup.

```bash
cmake --preset windows-x64          # or ubuntu-x86_64 / macos
cmake --build --preset windows-x64
```

GitHub Actions builds Windows, macOS, and Ubuntu on every push to `main`.

## License

GPL-2.0-or-later (required for OBS plugins that link libobs). Based on the OBS Plugin Template.
