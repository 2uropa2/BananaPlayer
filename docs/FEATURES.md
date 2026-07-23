# Vela Features

Vela is a minimal, keyboard-first Windows video player. This document catalogs
every user-facing feature currently shipped, derived from the source in this
repository. It is intended as raw material for a future product description or
help page - reading through it end-to-end should give a first-time reader a
complete sense of what Vela can do.

Version: **1.0.0** (`Version.hpp` / `VELA_VERSION`).

---

## Playback

| Feature | Invocation | Notes |
|---|---|---|
| Play / Pause | `Space` or single left-click on the video | Click also fires immediately for snappy feel; a follow-up double-click undoes the toggle so double-click is fullscreen-only. |
| Play / Pause (button) | Hero pill button in the bottom overlay | Icon flips between play triangle and pause bars. |
| Fullscreen toggle | `F`, `Enter`, `F11`, middle-click, or double-click on the video | Middle-click bypasses ImGui capture so it works over overlays too. |
| Exit fullscreen | `Esc` | Also closes any open modal panel first (Shortcuts / About / Info / playlist panel) before exiting fullscreen. |
| Stop | Stop icon in bottom overlay | Pauses and seeks to 0. |
| Playback speed - slower | `[` | Steps by 0.25x; clamped to 0.25 - 3.0x. |
| Playback speed - faster | `]` | Same step / clamp. |
| Playback speed - reset | `\` | Snaps back to 1.00x. |
| Playback speed - wheel | `Shift`+mouse wheel over window | Steps by 0.05x, same 0.25 - 3.0x clamp. |
| Playback speed - presets | Settings menu > **Playback Speed** | 0.5 / 0.75 / 1.0 / 1.25 / 1.5 / 2.0. |
| Speed indicator | Blue "N.NNx" pill left of the time text | Shown only when speed differs from 1.00x. |
| Frame step forward | `.` (period) | Uses 1/framerate as the step. |
| Frame step backward | `,` (comma) | Same step, seek-based. |
| Skip natural end for still images | Automatic | PNG / JPEG / BMP / TIFF / WebP / TARGA / JPEG 2000 / JPEG-LS / PCX / PPM / PGM / PBM / QOI decode as one-frame "videos" but do NOT auto-advance the playlist. Animated GIFs are treated as normal short videos. |
| Screensaver / monitor-sleep inhibit | Automatic | `SDL_DisableScreenSaver` while playing; `SDL_EnableScreenSaver` when paused, stopped, or no file loaded. Edge-triggered on the play state. |

## Seek

| Feature | Invocation | Notes |
|---|---|---|
| Seek +/- 5 s | `Left` / `Right` arrow keys | Auto-repeats on hold. |
| Seek +/- 5 s (side buttons) | Mouse `X1` (back) / `X2` (forward) | Works everywhere, including over overlays - checked before ImGui capture. |
| Seek +/- 5 s (wheel over seek bar) | Mouse wheel while cursor is over the seek bar | Wheel elsewhere adjusts volume. |
| Seek +/- 5 s (overlay buttons) | `<<` and `>>` icon buttons flanking the play button | |
| Seek +/- 30 s | `PgUp` / `PgDn` | Auto-repeats on hold. |
| Jump to start | `Home` | |
| Jump to near end | `End` | Lands 1 s before EOF to avoid instantly triggering natural-end. |
| Jump to N x 10 % | Number keys `0` - `9` (no modifiers) | 0 = start, 5 = halfway, 9 = 90 %. |
| Seek by dragging | Left-click and drag on the seek bar | Playback pauses on drag start and resumes on release. Rate-limited internally to ~120 ms so the demux thread doesn't thrash on every pixel; final position is always committed on release. |
| Previous chapter | `Alt+Left` | Jumps to start of current chapter if more than ~1.5 s in, otherwise to the previous chapter. Requires a container with chapters (MKV / MP4); OSDs "No chapters" if absent. |
| Next chapter | `Alt+Right` | Same requirement. Shows the chapter title in the OSD when present, otherwise "Chapter N / M". |
| Chapter tick marks | Automatic on seek bar | Sky-blue vertical posts per chapter start. Hovering within +/- 6 px of a tick shows a floating title tooltip above the bar. |
| A-B segment loop | `A` cycles through: set A, set B (must be after A), clear | Green post = A, orange post = B; the A-B span is shaded on the seek bar. The player automatically jumps back to A whenever playback crosses B. Cleared on file change. |
| Add bookmark | `Ctrl+M` | Inserts a bookmark at the current playhead. Deduped: `Ctrl+M` within 1 s of an existing mark is a no-op. Hard cap 50 marks per file. |
| Bookmark markers | Automatic on seek bar | Small amber diamonds below the track (chapters live ON the track as sky-blue posts, A-B markers extend above / below the track as green / orange posts) so all three marker classes stay distinct at a glance. Hover the diamond area for a `Bookmark  MM:SS` tooltip. |
| Bookmark picker | Settings menu > **Bookmarks** | Lists every mark for the currently-playing file; click a row to seek, `x` to remove that one, `Clear all` to wipe. `Add bookmark at current time` at the top is a duplicate of the `Ctrl+M` handler. |
| Bookmark persistence | Automatic | `vela.bookmarks` sidecar next to the exe. Format: `t1,t2,...\tpath` per line. MRU cap 200 files. Flushed to disk on file switch, close, and quit. |
| Seek OSD toasts | Debug builds only | `+ 5s`, `- 5s`, `+ 30s`, `- 30s`, `< step`, `step >` are suppressed in Release via the `SEEK_OSD` macro. |

## Audio

| Feature | Invocation | Notes |
|---|---|---|
| Volume +/- 5 % | `Up` / `Down` arrow keys | Auto-repeats on hold. Range 0 - 200 %. |
| Volume (wheel) | Mouse wheel over the video area | Same 5 % step. Auto-unmutes if you scroll up from 0. |
| Volume slider | Bottom overlay | Shows a "100" tick at unity; a "AMP N%" pill and orange fill light up when pushed above unity. |
| Volume boost | Up to 200 % | Above 100 % applied as `SDL_SetAudioStreamGain > 1` - can distort; the AMP indicator surfaces that. |
| Mute | `M` | Persistent. |
| Mute (button) | Speaker icon in bottom overlay | Icon flips between full / low / muted glyphs. |
| A/V sync offset | `Ctrl+Left` / `Ctrl+Right` | +/- 30 ms per press. Positive = delay video (audio heard late). Applied inside the render path, not the master clock, so seek/pause math stays clean. |
| A/V sync reset | `Ctrl+0` | Back to 0 ms. |
| A/V sync scope | Per-file transient | Explicitly NOT persisted; resets to 0 on every file switch. |
| Audio track picker | Settings menu > **Audio Track** | Lists every audio stream in the container; menu is only enabled when the file has more than zero audio tracks. |

## Subtitles

| Feature | Invocation | Notes |
|---|---|---|
| Auto-load companion subtitles | On file open | Vela scans for `<basename>*.srt` and `<basename>*.smi` next to the video. The first match loads automatically; every discovered track is exposed in the picker. |
| SRT parser | Automatic | Handles CRLF, BOM, dot-ms timestamps, out-of-order cues, missing index numbers. |
| SMI parser | Automatic | Case-insensitive tags, `<br>` -> newline, entity decoding, empty-body clear semantics, per-class filtering. |
| Encoding | Automatic | UTF-8 first, CP949 (Korean codepage) fallback for legacy files. |
| Drag-drop subtitle | Drop a `.srt` or `.smi` onto a loaded video | Swaps in the cues without touching playback state, VLC-style. |
| Load subtitle dialog | Settings menu > **Subtitles > Load Subtitle...** | Win32 file picker filtered to .srt / .smi. Disabled when no video is loaded. |
| Enable / disable overlay | `S` | Persisted as `subtitles_enabled` in `vela.config`. |
| Cycle subtitle size | `G` | Small / Medium / Large / XL. Persisted as `subtitle_size`. |
| Pick subtitle size | Settings menu > **Subtitles > Size** | Same four sizes. |
| Subtitle sync +/- 100 ms | `Ctrl+Shift+Left` / `Ctrl+Shift+Right` | Positive = show subs later. Clamped to +/- 10 s. Persisted as `subtitle_offset`. |
| Subtitle sync reset | `Ctrl+Shift+0` | Back to 0 ms. |
| Vertical position | `Alt+Up` / `Alt+Down` | Raises / lowers the subtitle line as a fraction of viewport height. Clamped to -15 % (down) / +60 % (up). Persisted as `subtitle_vpos`. |
| Vertical position reset | `Alt+0` | Back to default. |
| Background box | `B` | Toggles a semi-transparent rounded rect behind the subtitle text (helps on busy backgrounds where the outline alone isn't enough). Persisted as `subtitle_bg`. |
| Multi-track picker | Settings menu > **Subtitles > Track** | Only shown when the current video has more than one sidecar (e.g. `movie.ko.srt` + `movie.en.srt`). |
| Font | Merged with Malgun Gothic | Korean glyphs render correctly out of the box. |
| Rendering | White text with 8-way dark outline | Cheap but readable over any content. |

## Video

| Feature | Invocation | Notes |
|---|---|---|
| Aspect | Automatic fit-to-window | Always preserves source aspect. No forced 16:9 / 4:3 / 21:9 / Stretch modes - intentionally removed. |
| Intrinsic rotation | Automatic | Reads the container's `AV_PKT_DATA_DISPLAYMATRIX` side data (with a `rotate` metadata fallback), so portrait phone videos come out portrait without the user touching anything. |
| User rotation cycle | `Ctrl+R` | Steps through 0 / 90 / 180 / 270 degrees clockwise. Stacks with intrinsic rotation (intrinsic first, then user, mod 360). |
| User rotation pick | Settings menu > **Rotation** | Same four values. |
| User rotation scope | Per-file transient | Explicitly NOT persisted; resets to 0 deg on every file switch. |
| Fullscreen | `F` / `Enter` / `F11` / middle-click / double-click | Cursor auto-hides after ~1 s of idle in fullscreen; overlay bar hides after 3 s. In windowed mode the cursor is always visible. |
| Snap window to video aspect | `Ctrl+=` | Resizes the client area to the video's post-rotation aspect, filling up to 85 % of the current display's usable bounds. Un-maximizes first. No-op with an OSD in fullscreen or with no video loaded. |
| Overlay auto-hide | Automatic | Bar and top title fade out after 3 s of true idle (mouse movement, keys, wheel, clicks); fade in is snappier than fade out. Splash screen keeps overlays visible. |
| Hardware decode | Automatic (D3D11VA -> DXVA2) | Tries D3D11VA first, then DXVA2. If any HW step fails, transparently falls back to software decode. Shows a `HW decode  D3D11VA` OSD toast on successful HW attach. |
| No green flash on load | Automatic | The YUV texture draw is gated on the first successful frame upload, so the dark clear color fills the tiny gap between file-open and first-decoded-frame instead of an uninitialized green flash. |

## YouTube

| Feature | Invocation | Notes |
|---|---|---|
| Paste and play a YouTube URL | `Ctrl+V` | Reads `CF_HDROP`, then `CF_UNICODETEXT` from the clipboard. YouTube page URLs route through the yt-dlp resolver; other URLs go straight to FFmpeg; file paths and folders behave like drag-drop. |
| Recognized YouTube URL forms | Automatic | `youtube.com/watch`, `youtube.com/shorts/`, `youtube.com/live/`, `youtube.com/embed/`, `youtube.com/v/`, `youtu.be/`, with optional `www.` / `m.` / `music.` prefix. Case-insensitive. |
| Command-line YouTube | `Vela.exe <youtube-url>` | Single YouTube URL argument bypasses the file/folder expansion path and goes through the resolver. |
| yt-dlp bundling | NOT bundled | Vela expects `yt-dlp.exe` either next to `Vela.exe` (portable drop) or on `PATH`. The portable drop takes precedence. |
| yt-dlp availability status | Media Info modal (`I`) > YouTube section | Shows `detecting...`, or the version + age in days + resolved path, or `not installed` with a `winget install yt-dlp` hint. |
| yt-dlp "not installed" OSD | Automatic on first YouTube paste when missing | `yt-dlp not found - install: winget install yt-dlp`. |
| yt-dlp outdated nudge | Automatic (once per session) | If the exe is more than 30 days old, shows `yt-dlp is N days old - winget upgrade yt-dlp` on the first YouTube resolve of the session. |
| Quality selector - top overlay pill | Click the `YT  Auto` / `YT  1080p` / etc. pill that appears while a YouTube-resolved stream is playing | Opens a popup with the 5 quality choices. Re-resolves the same source at the new quality (playback position is lost). |
| Quality selector - menu | Settings menu > **YouTube Quality** | Auto (best) / 1080p / 720p / 480p / Audio only. Single-URL formats only. Persisted as `youtube_quality` in `vela.config`. |
| Resolve cancellation | Automatic on new request | A new resolve calls `TerminateProcess` on any in-flight yt-dlp child so only the newest result reaches the drain. |
| Resolve status OSDs | Automatic | `Resolving YouTube...`, `YouTube resolve failed: <message>`, or `YouTube resolver unavailable`. |

## Playback history

| Feature | Invocation | Notes |
|---|---|---|
| Open history modal | `Ctrl+H` or Settings menu > **Playback History** | Modal panel over the video; press `Esc` or `Ctrl+H` again to close. |
| List format | Automatic | Merged MRU of resume-marked files (with progress bar showing `pos / duration`) and recent-opened files without progress. Hover a row for the full path. |
| Continue watching | Click a row | Closes the modal and loads the file, restoring the resume position via the existing resume-index mechanism. |
| Remove one entry | `x` button on a row | Deletes both the resume mark and any Recent-list entry for that path. |
| Clear all | `Clear all history` button | Wipes `resume_index_` and `recent_files_` in one step. |
| Persistence | Automatic | Driven by the existing `vela.resume` and `vela.recent` sidecars - no new file. |

## Files & Playlist

| Feature | Invocation | Notes |
|---|---|---|
| Open File | `Ctrl+O`, splash "Open File..." button, or Settings menu > **Open File...** | Win32 file dialog; filter covers 12 video + 8 image extensions. |
| Open Folder | `Ctrl+Shift+O` or Settings menu > **Open Folder...** | Pick any file inside the folder; Vela scans siblings and enqueues everything matching the video/image extension whitelist. |
| Open Recent | Settings menu > **Open Recent** submenu | Up to 12 most-recent files, plus a Clear entry. Also surfaced as the splash "RECENT" list (up to 6 rows). |
| Paste URL / Path | `Ctrl+V` or Settings menu > **Paste URL / Path** | Handles YouTube URLs, generic URLs, file paths, folder paths, and file-drop tokens (CF_HDROP). Multi-line paste picks the first non-empty line. Strips wrapping `< >` / `" "` / `' '`. |
| Drop file(s) to play | Drag one or more files onto the window | First file starts playing; the rest go into the playlist. |
| Drop a folder | Drag a folder onto the window | Scans for videos/images and enqueues them, starting playback of the first. |
| Drop subtitle | Drag `.srt` / `.smi` onto a loaded video | Swaps subtitles without touching playback state. |
| Command-line multi-file | `Vela.exe path1 path2 ...` | First becomes the current playing file; the rest queue up. Directories expand into their video children. |
| Recent files list | Sidecar `vela.recent` | One absolute path per line, most recent first, capped at 12. |
| Resume position | Automatic (VLC-style) | Per-file playhead saved to `vela.resume` (cap 200, MRU). Restored only when `5 s < pos < duration - 10 s`. Flushed on every file switch and on natural end, so a crash doesn't lose it. Cleared when a file finishes naturally. |
| Session playlist restore | Automatic | Last-session playlist is saved to `vela.session.m3u` at exit and reloaded (but NOT auto-played) at next launch. |
| Save Playlist as M3U | Settings menu > **Save Playlist as M3U...** | Prompts for a file (default extension `.m3u8`). Writes `#EXTM3U` header + one absolute path per line. Disabled when playlist is empty. |
| Load Playlist (M3U) | Settings menu > **Load Playlist (M3U)...** | Clears existing playlist, ignores blank / `#`-prefixed lines, loads and plays the first entry. |
| Next in playlist | `N` or `Ctrl+N` or `>>|` icon in overlay | Skips any item that fails to open, bounded by playlist size. |
| Previous in playlist | `Ctrl+B` or `|<<` icon in overlay | |
| Toggle playlist panel | `P` or playlist icon in overlay | Docked right-side panel. Width scales down from 340 px to as low as 260 px on narrow windows. |
| Reorder within playlist | `^` / `v` buttons per row (panel wider than 300 px) | Also **Move to top** / **Move to bottom** via right-click on a row. |
| Remove item | `x` button per row, `Delete` key when panel is visible, or right-click > **Remove** | If the currently-playing item is removed, the player jumps to the new "current" (next logical item, or the last one if the tail was removed). |
| Row context menu | Right-click a playlist row | **Play now**, **Move to top**, **Move to bottom**, **Reveal in Explorer** (`/select,` in explorer.exe), **Copy path** (CF_UNICODETEXT, UTF-8-widened), **Remove**. |
| Row tooltip | Hover a playlist row | Full absolute path (long filenames stay identifiable). |
| Clear playlist | Header **Clear** button in panel | Disabled when playlist is empty. |
| Right-click on video area | Anywhere on the video area outside a widget | Opens a context menu wired to the same `drawMainMenuContents` used by the Settings gear icon (so the two surfaces stay in sync automatically). |
| Repeat mode | `L` cycles Off / One / All, or Settings menu > **Repeat** | Persisted as `repeat_mode`. Repeat One replays the current file at EOF; Repeat All wraps (and reshuffles when shuffle is on). |
| Shuffle | `H` or Settings menu > **Shuffle** | Owns an internal permutation; current item is kept at the front so playback continues from where you are. Persisted as `shuffle`. |
| Close current file | `Ctrl+W` | Unloads the video, clears subtitle state, resets the title. Playlist stays intact so you can pick another entry without re-scanning. |
| Broken-item recovery | Automatic in playlist walks | `playNextInPlaylist` and `handleNaturalEnd` skip items that fail to open, bounded by playlist size so an all-broken queue eventually stops. |
| Stalled-open watchdog | Automatic | `AVIOInterruptCB` with a 5 s wall-clock deadline covers `avformat_open_input` + `avformat_find_stream_info`; disarmed for steady-state reads. |
| "Cannot open" OSD | Automatic on `loadFile` failure | Shown for 3 s, and the window title is reset to `Vela` so nothing lingers from the previous file. |
| Window title | Automatic | Format `filename  -  Vela` (VLC-style, filename first). |

### Supported file extensions

**Video:** `.mp4`, `.mkv`, `.avi`, `.mov`, `.webm`, `.wmv`, `.flv`, `.m4v`, `.mpg`, `.mpeg`, `.ts`, `.3gp`.

**Still images (via FFmpeg image2 demuxer):** `.png`, `.jpg`, `.jpeg`, `.gif`, `.bmp`, `.webp`, `.tif`, `.tiff`. (Animated GIFs play through their frames like a short video.)

**Subtitles:** `.srt`, `.smi`.

Extensions are wired identically into `UIController::isVideoFile`, the `openFileDialogWin` filter, and the installer's `resources/vela_reg_install.nsh` OpenWithList / SupportedTypes block.

## Snapshots

| Feature | Invocation | Notes |
|---|---|---|
| Save snapshot as PNG | `Ctrl+S` or Settings menu > **Snapshot** or Snapshot icon in overlay | Writes `snapshot_YYYYMMDD_HHMMSS.png` next to `Vela.exe`. A brief camera-flash overlay + "Saved  <filename>" OSD confirm. |
| Copy snapshot to clipboard | `Ctrl+Shift+S` | Writes the current frame as `CF_DIB` (Windows convention for image paste) so it can be pasted into any app. |
| Snapshot fails gracefully | Automatic | OSDs `Snapshot failed`, `Clipboard busy`, `Clipboard alloc failed`, or `Clipboard write failed` as appropriate. |

## Window

| Feature | Invocation | Notes |
|---|---|---|
| Minimum size | Automatic | 640 x 400. All UI is required to survive this width without clipping. |
| Default size | 1280 x 720 | On first launch. |
| Always on top | `T` or Settings menu > **Always on Top** | Toggles `SDL_SetWindowAlwaysOnTop`. Persisted as `always_on_top`. |
| App icon | Embedded | Multi-size (16 / 32 / 48 / 64 / 128 / 256) ICO baked into the exe via `resources/app.rc`; also pushed onto the SDL window's HWND via `WM_SETICON`. |
| Screensaver inhibit | Automatic | See Playback section. |
| Cursor auto-hide | Automatic in fullscreen only | ~1 s idle. Windowed mode always shows the cursor. |
| Window geometry restore | Currently disabled | The sidecar-file plumbing exists (`vela.window` with `x/y/w/h/maximized`) but restore is commented out pending a fix for the SDL_WINDOWPOS_CENTERED clamp bug. |

## Info & help

| Feature | Invocation | Notes |
|---|---|---|
| Media Info modal | `I` or Settings menu > **Media Info** | File name, absolute path, resolution, video codec, frame rate, decoder (HW device name or "Software"), rotation (intrinsic, degrees), audio codec, sample rate + channels, bit rate, duration. Plus a **YouTube** row showing yt-dlp detection state, version, age, and resolved path. Close with `Esc` or `I`. |
| Keyboard Shortcuts modal | `?` (Shift+/), `F1`, or Settings menu > **Keyboard Shortcuts** | Grouped by Playback / Seek / Sync / Audio / Speed / Files & tracks / View. Close with `Esc` or `?`. |
| About Vela modal | Settings menu > **About Vela** | VELA wordmark, version (from `VELA_VERSION`), stack line (SDL3 . Dear ImGui . FFmpeg), tagline "Focused, keyboard-first video playback." Close with `Esc` or the Close button. |
| Splash screen | Shown whenever no file is loaded | Large VELA wordmark with an accent-gradient underline, tagline, `Drop a video or image    Ctrl+O to open    Space to play` hint line, `?  or  F1  for keyboard shortcuts` hint line, big **Open File...** button, RECENT list (up to 6 rows), version + stack footer bottom-right. |
| OSD toasts | Right-hand side, top of the video | Play / Pause, Volume %, Speed Nx, Loop A/B, Repeat / Shuffle state, Rotation, sync nudges, Snapshot Saved, Clipboard messages, "No chapters", "Chapter: ..." etc. Slide-in animation, ~1.6 s default; stack top-down. |
| Snapshot flash | Automatic on snapshot | Brief bright white overlay, mimicking a camera-flash "pulse". |
| Right-click context menu | Right-click on video area | Same items as the Settings menu; anchored at the click point. |
| Playback rate badge | Automatic (bottom overlay) | Small blue "1.50x" pill left of the time text, visible only when speed differs from 1.00x. |
| Playlist position badge | Automatic (top overlay) | Small blue "N / M" pill next to the filename, visible only when playlist has more than 1 item. |
| YouTube quality pill | Automatic (top overlay) | Blue "YT  <quality>" pill visible only while a YouTube-resolved stream is playing. Click to change. |
| Seek-bar hover thumbnail | Settings menu > **Thumbnail preview on seek bar** | Off by default. Enable to get a floating video-frame preview above the cursor when hovering the seek bar (own AVFormatContext + worker thread + 8-entry LRU cache; 2 s bucket coalescing). Persisted as `thumbnail_preview`. |

## Persistence (sidecar files next to Vela.exe)

Vela stores its state in five sidecar files next to `Vela.exe`. All are UTF-8
plain text.

| File | Purpose |
|---|---|
| `vela.config` | Persistent user settings (see key list below). |
| `vela.resume` | Per-file resume positions. One entry per line, TAB-separated: `pos<TAB>duration<TAB>path`. MRU order, capped at 200 entries. Only positions passing `5 s < pos < duration - 10 s` are honored on re-open. |
| `vela.recent` | Recent-files list. One absolute path per line, most-recent first, capped at 12. Feeds the splash "RECENT" section and the Open Recent submenu. |
| `vela.session.m3u` | Last-session playlist snapshot. `#EXTM3U` + one absolute path per line. Restored (but NOT auto-played) at next launch. Deleted when the playlist is empty at exit. |
| `vela.window` | Window geometry sidecar (currently NOT wired up at runtime). Format: `x=`, `y=`, `w=`, `h=`, `maximized=`. |
| `snapshot_YYYYMMDD_HHMMSS.png` | Ctrl+S snapshots. One file per capture. |

### `vela.config` keys

All values are persisted on quit. Unknown keys parse cleanly (ignored) so
older configs survive future versions.

| Key | Type | Notes |
|---|---|---|
| `volume` | float | 0.0 - 2.0 (0 - 200 %). |
| `muted` | int (0/1) | |
| `subtitle_size` | int (0-3) | Small / Medium / Large / XL. |
| `subtitle_offset` | float (seconds) | Applied on subtitle lookup. |
| `subtitle_vpos` | float (fraction) | -0.15 to +0.60 of viewport height. |
| `subtitle_bg` | int (0/1) | Background box toggle. |
| `subtitles_enabled` | int (0/1) | Overlay on/off. |
| `always_on_top` | int (0/1) | |
| `repeat_mode` | int (0-2) | None / One / All. |
| `shuffle` | int (0/1) | |
| `thumbnail_preview` | int (0/1) | Seek-bar hover thumbnail. |
| `youtube_quality` | string | `auto` / `1080` / `720` / `480` / `audio`. |
| `av_sync_offset` | (parsed, ignored) | Per-file transient; kept only so old configs parse. |
| `rotation` | (parsed, ignored) | Per-file transient; kept only so old configs parse. |

## Requirements & environment

- **OS:** Windows 10 / 11 (uses Win32 file dialogs, clipboard, registry, icon handling; `snapshot_YYYYMMDD_HHMMSS.png` path style). Cross-platform is an explicit non-goal for now.
- **Runtime dependencies (bundled with installer / ZIP):** SDL3, FFmpeg (`av*`, `sw*`), and the Visual C++ redistributable.
- **yt-dlp (optional):** Required only for YouTube playback. Vela looks for it in this order:
  1. `yt-dlp.exe` next to `Vela.exe` (portable drop takes precedence).
  2. Anywhere on `PATH` (resolved via `where.exe yt-dlp.exe`).
  If neither is present, YouTube URLs are refused with an actionable `winget install yt-dlp` OSD and the Media Info modal shows `not installed`.
- **File associations:** The NSIS installer registers Vela as a secondary "Open with" handler for every video and image extension listed above (does NOT displace the default handler). Also registers `FriendlyAppName`, `DefaultIcon`, `shell\open\command`, and an App Paths entry so `Win+R -> Vela` works. Full list of registry keys is in `resources/vela_reg_install.nsh` and CLAUDE.md.
- **Packaging:** CPack produces a `Vela-<ver>-win64.exe` NSIS installer with Start Menu + Desktop shortcuts, previous-version cleanup, and an uninstaller, plus a `Vela-<ver>-win64.zip` portable bundle. Both contain every runtime DLL.

## Build variants

- **Release** links as `WINDOWS` subsystem (no console) and defines `NDEBUG`, which suppresses the noisy seek OSDs (`+/- 5s`, `+/- 30s`, `< step`, `step >`) for uncluttered normal viewing.
- **Debug** keeps the console (SDL / FFmpeg stderr visible) and shows every seek OSD - useful when investigating timing bugs.
