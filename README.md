# Banana Player

**Peel and play.** A focused, keyboard-first Windows video player built entirely by an
agentic coding workflow.

- Version: **1.4.0** (see [`Version.hpp`](Version.hpp))
- Platform: Windows 10 / 11 (x64)
- Repo: <https://github.com/2uropa2/BananaPlayer>

---

## Why this exists

Banana Player is a real, shipping desktop application whose every line of code was
produced by an AI assistant (Claude) under natural-language direction. The human owner
writes zero code by hand. The goal of the experiment is to see whether a genuinely
polished product - not a toy demo - can be built and evolved that way.

Product-wise, Banana Player targets one thing: **frictionless single-user playback**.
No title bar. No docked panels. No skinning. No cloud accounts. Overlays float on the
video and fade out after three seconds. Every mouse action has a keyboard shortcut.
It opens, it plays, it gets out of the way.

See [`CLAUDE.md`](CLAUDE.md) for the full project charter and design principles, and
[`docs/WORK_HISTORY.md`](docs/WORK_HISTORY.md) for the development timeline.

---

## Highlights

### Playback engine
- **SDL3 + FFmpeg + Dear ImGui** stack; C++17.
- **Hardware decode** by default (D3D11VA -> DXVA2), with a full software fallback
  so unsupported codecs still play.
- **MediaClock sync architecture**: wall-clock anchored master time, nudged toward
  the true audio-heard position on every audio push. Eliminates the "close but not
  quite" A/V drift that per-file latency tuning can never fully paper over.
- **Three-thread pipeline** (demux + video decode + audio decode) with bounded
  per-stream packet queues. Survives files whose demuxer emits long video-only runs
  before the first audio packet (Twitter mp4s with negative-DTS edit lists).
- **CPU HDR tone mapping** (PQ / HLG) with hand-written BT.2020 -> BT.709 pipeline
  and cached LUTs. 4K HDR at realtime on a modern desktop.
- **Frame stepping** in both directions, backed by a decoded-frame ring buffer so
  backward step is seek-less within the recent window.
- **Playback rate** 0.25x - 3.0x via `SDL_SetAudioStreamFrequencyRatio` (audio pitch
  shifts - deliberate simplification, no SoundTouch dependency).
- **Screensaver / monitor-sleep inhibit** while playback is active.

### Media support
- All standard container / codec combinations FFmpeg supports.
- Still images (PNG, JPEG, BMP, TIFF, WebP, TARGA, JPEG 2000, JPEG-LS, PCX, PPM/PGM/PBM,
  QOI) decoded through the same video pipeline; single-frame images don't auto-advance.
- Animated GIFs treated as short videos.
- External subtitles: SRT + SMI, UTF-8 first with a CP949 fallback for legacy
  Korean sidecars.
- Embedded MKV / MP4 subtitle tracks with priority-based auto-selection
  (Korean text -> any text -> Korean bitmap -> any bitmap).
- YouTube URLs resolved via `yt-dlp` (if present on `PATH`).

### UX
- **Overlay auto-hide**: 3 s idle - windowed and fullscreen alike. Only real input
  (mouse motion, key/wheel/click, active drag, open popup, open playlist) resets
  the timer. Cursor auto-hides only in fullscreen.
- **Hero controls**: one big Play/Pause pill; snappy fade-in, gentler fade-out.
- **Fits `640 x 400`**: every layout survives the minimum window size without
  clipping.
- **Right-click** anywhere on the video for the main menu; **left-click the gear
  icon** for the Settings dialog (six tabs: Playback / Video / Audio / Subtitles /
  Snapshots / Behavior).
- **Persistence**: per-file resume position (VLC-style), recent files list, last
  session playlist, all preferences. Sidecars live next to the exe as
  `banana.config`, `banana.resume`, `banana.recent`, `banana.session.m3u`,
  `banana.bookmarks`, `banana.window`, `banana.stats`, `banana.hdrlut`.
- **Auto-migration** from the previous "Vela" branding: any `vela.*` sidecar found
  on first launch is renamed to `banana.*` so existing users keep their history.

---

## Keyboard shortcuts (essentials)

| Action | Shortcut |
|---|---|
| Play / Pause | `Space` |
| Fullscreen | `F` / `Enter` / `F11` / double-click |
| Seek +/- 5 s | `Left` / `Right` (also mouse `X1` / `X2`) |
| Seek +/- 30 s | `PgUp` / `PgDn` |
| Jump 0 % - 90 % | `0` - `9` |
| Frame step | `,` / `.` |
| Volume +/- 5 % | `Up` / `Down` (also mouse wheel over video) |
| Mute | `M` |
| Loudness normalizer | `Shift+N` |
| Speed slower / faster / reset | `[` / `]` / `\` |
| A-B loop | `A` cycles A / B / clear |
| Subtitle overlay | `S` |
| Subtitle size | `G` |
| Subtitle sync +/- 100 ms | `Ctrl+Shift+Left` / `Right` (reset `Ctrl+Shift+0`) |
| Audio track cycle | `Ctrl+A` |
| A/V sync +/- 30 ms | `Ctrl+Left` / `Right` (reset `Ctrl+0`) |
| Snapshot (PNG next to exe) | `Ctrl+S` |
| Snapshot to clipboard | `Ctrl+Shift+S` |
| Burst snapshots | `Ctrl+Shift+G` |
| Rotate 90 deg | `Ctrl+R` |
| Zoom / pan | `Ctrl+Wheel` / `Alt+Drag` (reset `Alt+0`) |
| Playlist next / prev | `N` / `Ctrl+B` |
| Repeat / Shuffle | `L` / `H` |
| Open file / folder | `Ctrl+O` / `Ctrl+Shift+O` |
| Playlist panel | `P` |
| Always on top | `T` |
| Picture-in-Picture | `Shift+P` |
| Media Info | `I` |
| Shortcuts panel | `?` / `F1` |
| Settings dialog | `Ctrl+,` (or click the gear) |
| Close file | `Ctrl+W` |

Full catalog: [`docs/FEATURES.md`](docs/FEATURES.md).

---

## Install

### From release (recommended)

Download `BananaPlayer-<version>-win64.exe` (NSIS installer) or the portable
`.zip` from the [Releases page](https://github.com/2uropa2/BananaPlayer/releases).
The installer registers Start Menu / Desktop shortcuts, an uninstaller, and
"Open With" entries for the video / image formats Banana Player supports (never
displaces existing default handlers).

### Build from source

```powershell
# Prereqs: vcpkg with the following ports installed
#   sdl3
#   imgui[sdl3-binding,sdl3-renderer-binding]
#   ffmpeg
# Assumes VCPKG_ROOT env var or vcpkg at C:\vcpkg.

cmake -S . -B build
cmake --build build --config Release

build\Release\BananaPlayer.exe path\to\video.mp4
```

`SDL3.dll`, the FFmpeg DLLs, and the VC++ redistributable are auto-copied next to
the exe by vcpkg's applocal step. `main.cpp` accepts zero or more path arguments;
extras become the initial playlist.

### Packaging

```powershell
# Requires NSIS on PATH (winget install NSIS.NSIS).
cd build
cpack -C Release -G "ZIP;NSIS"
```

Produces `BananaPlayer-<version>-win64.exe` and `BananaPlayer-<version>-win64.zip`.
Both bundle every runtime DLL the exe needs.

### Tests

```powershell
cmake --build build --config Release --target banana_tests
build\Release\banana_tests.exe
```

doctest suite covering the SRT / SMI parsers and their tricky edge cases (BOMs,
CRLF, dot-ms timestamps, out-of-order cues, SMI class filtering, entity decoding,
CP949 fallback, garbage input).

---

## File layout

```
main.cpp                SDL / ImGui bootstrap, main loop, YUV upload, letterbox, Win32 icon
VideoPlayer.{hpp,cpp}   FFmpeg demux + decode threads, MediaClock, audio stream, snapshots
UIController.{hpp,cpp}  All ImGui overlays, keyboard / mouse / drop handling, persistence
Style.{hpp,cpp}         Theme, vector icons, custom SeekBar / IconButton / TextButton
Playlist.hpp            List + index + repeat / shuffle
Subtitles.{hpp,cpp}     SRT / SMI parsers - deliberately isolated for unit testing
Thumbnail.{hpp,cpp}     Seek-bar frame preview
License.{hpp,cpp}       License-key acceptance surface
Promo.{hpp,cpp}         In-app promo panel
Youtube.{hpp,cpp}       yt-dlp shell-out
Version.hpp             Single source of truth for BP_VERSION
resources/app.rc        Windows resource script (icon + version info)
resources/icon.ico      Multi-size app icon
resources/banana_reg_install.nsh + banana_reg_uninstall.nsh
                        NSIS registry writes for "Open With" registration
tests/test_*.cpp        doctest suite
tools/                  install / icon-generation / CI / version-bump scripts
docs/                   FEATURES / BRAND / HARNESS / MONETIZATION / WORK_HISTORY
```

---

## Non-goals

- Cross-platform (Linux / macOS) - not until asked.
- Skinning / theme editor - the warm dark banana theme is the theme.
- Network streaming beyond what FFmpeg / yt-dlp deliver for free.
- EQ / video filters (color grading, sharpen, etc.).
- Pitch-preserving speed change (would need SoundTouch / rubberband; current
  chipmunk-audio approach is deliberate).
- Styled ASS / SSA subtitles, PGS bitmap subtitles.

---

## License

Banana Player's own code: **MIT** (see [`LICENSE`](LICENSE)).

Third-party components (SDL zlib, FFmpeg LGPL, ImGui MIT, yt-dlp Unlicense, MSVC
redistributable): see [`LICENSE-3RDPARTY`](LICENSE-3RDPARTY).

---

## Docs

- [`CLAUDE.md`](CLAUDE.md) - project charter, design principles, evolution log,
  landmines list. If you (or an AI assistant) are picking up the project fresh,
  start here.
- [`CHANGELOG.md`](CHANGELOG.md) - per-release notes.
- [`docs/FEATURES.md`](docs/FEATURES.md) - exhaustive feature catalog.
- [`docs/WORK_HISTORY.md`](docs/WORK_HISTORY.md) - development timeline organized
  for session-to-session continuity.
- [`docs/BRAND_BANANA.md`](docs/BRAND_BANANA.md) - brand identity (name, palette,
  tagline).
- [`docs/HARNESS.md`](docs/HARNESS.md) - dev harness / tools reference.
- [`docs/MONETIZATION_PLAN.md`](docs/MONETIZATION_PLAN.md) - the plan for turning
  the experiment into a product.
