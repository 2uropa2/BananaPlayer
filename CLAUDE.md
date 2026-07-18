# Vela

## Project charter

**Vela** is the product name. The repository lives at `C:\work\Player` for historical reasons — the project was renamed after the core UX was working. Every internal identifier (exe, install dir, CMake target, package name, registry entries) uses `Vela` / `Vela.exe`.

This project is an **experiment in 100% agentic coding**: the human owner writes zero code by hand. Every line here is produced by Claude via natural-language direction, and the goal is to see whether a real, shippable desktop product can be built and evolved that way.

Practical implications for future sessions:

- The user does not review diffs line-by-line — they judge by *running the app*. Any change you land must build cleanly and behave sensibly at runtime, not just pass the compiler.
- Prefer **small, coherent edits** the user can eyeball in one session over sweeping refactors they can't verify.
- When in doubt about direction, ship a plausible best guess and iterate. Do not stall on clarification questions unless the choice is truly irreversible.
- Treat this as a *product*, not a demo: crashes, layout bugs, off-screen controls, and unresponsive input are all first-class defects.
- The user leans on you as a **video-player UX expert**. When they ask "what does this need?", propose a shortlist of high-impact features (standard-player behaviors first — resume position, mouse-wheel volume, speed control, keyboard-first shortcuts). Ship them in small verifiable slices.

## Stack

- **C++17**
- **SDL3** — window, renderer, audio output, input
- **Dear ImGui** (SDL3 + SDLRenderer3 backends) — all UI overlays
- **FFmpeg** (libavformat / libavcodec / libswscale / libswresample) — demux + decode
- **vcpkg** — dependency manager
- **CMake ≥ 3.21** with the vcpkg toolchain (3.21 for `TARGET_RUNTIME_DLLS` and modern install helpers)

## Build

Windows-only right now (uses Win32 `GetOpenFileName`, `LoadImageW`/`WM_SETICON` for the window icon, and `snapshot_YYYYMMDD_...` path style).

```powershell
# One-time: vcpkg with SDL3, imgui[sdl3-binding,sdl3-renderer-binding], ffmpeg installed.
# Assumes VCPKG_ROOT env var or vcpkg at C:\vcpkg.

cmake -S . -B build
cmake --build build --config Release
build\Release\Vela.exe path\to\video.mp4
```

`SDL3.dll` and the FFmpeg DLLs are auto-copied next to the exe by vcpkg's applocal step. `main.cpp` accepts an optional path argument.

### Packaging

```powershell
# Requires NSIS on PATH (installed via `winget install NSIS.NSIS`).
cd build
cpack -C Release -G "ZIP;NSIS"
```

Produces `Vela-<ver>-win64.exe` (NSIS installer with Start Menu + Desktop shortcuts, uninstaller, previous-version cleanup) and `Vela-<ver>-win64.zip` (portable). Both bundle every runtime DLL the exe needs — SDL3, FFmpeg (av*, sw*), and the VC++ redistributable.

### Debug vs Release

- **Release** builds with the `WINDOWS` subsystem — no console tags along with the GUI. Also `NDEBUG` is defined, which suppresses the noisy seek OSDs (`± 5s`, `± 30s`, `< step`, `step >`) so they don't clutter normal viewing.
- **Debug** keeps the console (so `stderr` from SDL/FFmpeg is visible) and shows every seek OSD — useful when investigating timing bugs.
- Never remove the `SEEK_OSD` macro or the `#ifdef _WIN32` / `WIN32_EXECUTABLE` split without asking — the user explicitly wanted these gated.

## File map

- `main.cpp` — SDL/ImGui bootstrapping, main loop, YUV texture upload, letterbox math, Win32 icon injection
- `VideoPlayer.{hpp,cpp}` — FFmpeg demux/decode thread, A/V clock, audio stream, snapshots, playback rate
- `UIController.{hpp,cpp}` — all ImGui overlays, keyboard/mouse/drop event handling, playlist glue, resume-position persistence
- `Style.{hpp,cpp}` — theme, vector icons, custom `SeekBar` / `IconButton` / `TextButton`
- `Playlist.hpp` — trivial header-only list + index
- `Subtitles.{hpp,cpp}` — pure SRT / SMI parsers, UTF-8 validation, CP949 fallback decode, and companion-file loader. Deliberately isolated so `tests/test_subtitles.cpp` can hit it without any SDL / FFmpeg deps.
- `resources/app.rc` — Windows resource script: embeds `icon.ico` and version info in the exe
- `resources/icon.ico` — multi-size (16/32/48/64/128/256) PNG-in-ICO app icon
- `tools/generate_icon.ps1` — regenerates `resources/icon.ico` via PowerShell + System.Drawing. Run only when tweaking the icon design.
- `tests/test_main.cpp` + `tests/test_subtitles.cpp` — doctest suite. Built with `-DVELA_BUILD_TESTS=ON` (default). Fetched header is dropped into `build/doctest_include/doctest/doctest.h`.

Installer file-association registry keys — the raw NSIS commands live in `resources/vela_reg_install.nsh` (and its uninstall twin) and are pulled into the installer via `!include`. **Never inline these commands into `CPACK_NSIS_EXTRA_INSTALL_COMMANDS` as a big CMake string.** CPack writes that value into `CPackConfig.cmake` and NSIS reads it via template substitution, and the CPack side does NOT escape the literal `"` characters that `shell\open\command` needs (`"$INSTDIR\bin\Vela.exe" "%1"`). What actually reaches NSIS is the value *without* the inner quotes — Windows tries `C:\Program.exe` first, fails, and Explorer either drops Vela from the "Open with" list or lists it as an unnamed entry that errors on click. The `!include` route bypasses the round-trip entirely: NSIS reads the .nsh at compile time with its own quoting rules intact. Also: the include path must be native (backslashes) and space-free — see the pre-flight check in `CMakeLists.txt`.

The keys the .nsh writes (all required — dropping any one degrades "Open with" behavior):
- `HKCR\Applications\Vela.exe` — `FriendlyAppName = "Vela"`
- `HKCR\Applications\Vela.exe\DefaultIcon` — `"$INSTDIR\bin\Vela.exe",0` (icon shown in the dialog)
- `HKCR\Applications\Vela.exe\shell\open\command` — `"$INSTDIR\bin\Vela.exe" "%1"` (how Explorer actually launches it; the earlier v0.1 installer skipped this and that's why Vela didn't appear in "Open with" at all)
- `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\Vela.exe` — Default + `Path` (for `Win+R -> Vela` and any tool that resolves via App Paths)
- `HKCR\Applications\Vela.exe\SupportedTypes` — one empty value per video extension (Vista+ registration path)
- `HKCR\{.ext}\OpenWithList\Vela.exe` — one subkey per extension (classic Open With list)

Never trim this set thinking "OpenWithList alone is enough" — it isn't. The uninstaller mirrors every write.

Runtime side-files created next to the exe:
- `snapshot_YYYYMMDD_HHMMSS.png` — Ctrl+S snapshots
- `vela.resume` — per-file resume positions (`pos<TAB>duration<TAB>path`, MRU, cap 200)
- `vela.config` — key=value settings (volume, subtitle size/offset/enabled, always-on-top, repeat mode, shuffle, A/V sync, rotation)
- `vela.recent` — recent-files list (one absolute path per line, MRU, cap 12) — feeds the splash "RECENT" section and the Open Recent menu
- `vela.session.m3u` — last-session playlist snapshot restored on next launch (does NOT auto-play — the user still has to pick something)

## Design principles

These reflect choices already baked in — keep future changes consistent with them unless the user asks otherwise.

- **Overlays over chrome.** No title bar / menu bar / docked panels. Controls are semi-transparent overlays that fade in on activity and fade out after 3 s idle **in both windowed and fullscreen** (the immersive default; splash keeps overlays visible). Fade-in is snappier than fade-out (speed 22 vs 6).
- **Content-first layout.** The video always fills the available window at the correct aspect; UI floats on top and never displaces it.
- **Layouts must fit the minimum window (`640×400`)**. Any new control has to survive that width without clipping. Prefer right-alignment via `ImGui::GetContentRegionAvail()` / `SetCursorPosX()` over hand-counted pixel widths — the latter is what caused the fullscreen-button-off-screen bug.
- **One primary action per surface.** Splash has one big "Open File…" button + Recent list. Bottom bar has one big Play/Pause. Everything else is secondary iconography.
- **Icons are hand-drawn vectors** (see `Style::DrawIcon`) — no icon fonts, no image assets. Add new icons to the `Style::Icon` enum and the `DrawIcon` switch. The exe icon is a separate story (see `resources/`).
- **Keyboard-first, mouse-wheel-aware.** Every mouse action should have a shortcut, and standard player wheel/hold behaviors are expected. Current bindings:
    - **Playback:** Space (play/pause), F / Enter / F11 (fullscreen), **middle-click / double-click** (toggle fullscreen; middle-click bypasses `io.WantCaptureMouse` for the same reason X1/X2 do — no widget uses the middle button), Esc (exit fullscreen or close panel)
    - **Seek:** ←/→ ±5 s, PgUp/PgDn ±30 s, **mouse side buttons X1 / X2** ±5 s (back / forward — the standard 5-button-mouse "browser back/forward" pair, routed to seek because a video player has nothing else to do with them; kept at 5 s not 10 s so all "small nudge" surfaces feel consistent). Home / End (jump to start / near end), `,` / `.` (frame step). Side buttons are checked BEFORE `io.WantCaptureMouse` so they still seek when the cursor is over an overlay. **Number keys 0-9** jump to N × 10 % of the file (0 = start, 5 = halfway, 9 = 90 %). **Mouse wheel over the seek bar** seeks ±5 s (wheel elsewhere still adjusts volume).
    - **Volume:** ↑/↓ ±5 %, M (mute), **mouse wheel over video area** = volume. Range 0 – 200 % (boost above unity works via `SDL_SetAudioStreamGain > 1`).
    - **Speed:** `[` slower, `]` faster (0.25 step, clamp 0.25 – 3.0), `\` reset to 1.0x
    - **Loop / subs:** A (cycle A / B / clear for A-B segment loop), S (toggle subtitle overlay)
    - **A/V sync:** Ctrl+Left / Ctrl+Right shift the video by -/+ 30 ms per press (positive = delay video, use when audio is heard late), Ctrl+0 resets to 0 ms. Offset persists through pause/seek and resets on file load. Applied inside `VideoPlayer::updateTexture` — not to the master clock itself — so the audio path, natural-end detection, and seek math stay untouched.
    - **Subtitle sync:** Ctrl+Shift+Left / Ctrl+Shift+Right nudge the sub display time by ±100 ms per press (positive = show subs later). Ctrl+Shift+0 resets. Applied inside `UIController::currentSubtitle` by subtracting from the lookup timestamp — cheap, no cue re-scan needed. Persists in `vela.config`.
    - **Subtitles:** S toggles overlay, G cycles size (Small / Medium / Large / XL).
    - **Files / playlist:** Ctrl+O (open), Ctrl+Shift+O (open folder → auto-load videos into playlist), Ctrl+S (snapshot), P (playlist panel), L (cycle repeat: Off / One / All), H (shuffle), N / Ctrl+N (next), Ctrl+B (prev).
    - **Aspect:** none — the render loop always fits the source aspect to the window. Forced 16:9/4:3/21:9 modes and Stretch were removed on 2026-07-18 after the user pointed out that distorting the picture to a shape it isn't is never what a viewer actually wants; 1-5 number keys are now free for future use. **Rotation:** Ctrl+R cycles 0 / 90 / 180 / 270 (real re-orientation, not aspect coercion). The render pipeline also applies the container's intrinsic rotation (`VideoPlayer::getIntrinsicRotation()` — reads the `AV_PKT_DATA_DISPLAYMATRIX` side data on the video stream's `codecpar`, falls back to the legacy `rotate` metadata tag) so portrait phone videos come out portrait without the user touching anything. Intrinsic + user rotation are summed mod 360.
    - **Always on top:** T toggles `SDL_SetWindowAlwaysOnTop`.
    - **Info modal:** I opens the Media Info panel (codec, resolution, framerate, bit rate, HW decoder, sample rate/channels, duration). Esc / I closes.
    - **Fullscreen alt:** F11 (in addition to F / Enter) — matches every browser and most players.
    - **Drop a file** = play immediately (not enqueue). **Dropping a .srt or .smi onto a loaded video** swaps in the sidecar cues without touching playback state (like VLC).
    - **Ctrl+W** closes the current file (keeps the playlist so the user can pick another entry without re-scanning).
    - **Ctrl+Shift+S** copies the current frame to the clipboard as a `CF_DIB` (Windows convention for image paste). Regular Ctrl+S still writes a PNG next to the exe.
    - **Right-click on the video area** opens a context menu wired to the same `drawMainMenuContents()` used by the Settings gear icon, so both surfaces stay in sync automatically.
    - **Delete** removes the current playlist entry (only when the playlist panel is visible — outside that context Delete does nothing, so it never surprises the viewer).
    - **Window geometry (position/size/maximized) persists in `vela.window`** next to the exe. Loaded in ctor after settings; saved in dtor. Fullscreen state is intentionally NOT persisted — we save the windowed geometry so the next launch reopens where the user last had it, and a monitor-unplug clamp keeps the window on-screen if the saved position is now off-desktop.
    - **Screensaver + monitor sleep** are inhibited via `SDL_DisableScreenSaver` while playback is active, and re-enabled when paused or stopped. The main loop tracks the edge on `player.isPlaying()` and only calls SDL when the state actually changes.
    - **Command line** accepts one *or more* file paths; extra args become the playlist and the first one starts playing.
    - **Playlist panel** supports reorder (^ / v per row), remove (x), and Clear (header) — mutations are deferred to end-of-frame so ImGui row IDs stay stable during the loop.
- **Hold-to-repeat is a whitelist.** Only ←/→ *without Ctrl*, PgUp/PgDn, ↑/↓ act on `event.key.repeat`; everything else (Space, F, Ctrl combos, `[`/`]`, letters) fires once per press. Ctrl+←/→ and Ctrl+Shift+←/→ are explicitly excluded from the holdable set — otherwise holding those keys spams A/V sync (30 ms) or subtitle sync (100 ms) nudges at OS repeat rate and silently racks up seconds of drift. Never blanket-block or blanket-allow repeats.
- **Do NOT set `ImGuiConfigFlags_NavEnableKeyboard`.** With it on, clicked overlay buttons retain focus and steal Space / Enter, so pressing Space re-triggers whichever button was last clicked (e.g. Snapshot instead of pause). All shortcuts are handled directly in `UIController::handleEvent` — no ImGui keyboard nav needed.
- **Decode runs on its own thread**, main thread only uploads frames + renders. Never block the UI thread on FFmpeg calls.
- **Audio is the master clock** when present, wall-clock fallback otherwise. Don't add features that assume video-driven timing.
- **Playback rate** is applied two ways: `SDL_SetAudioStreamFrequencyRatio` on the audio stream (pitch-shifted — simple, no SoundTouch dependency), and a rate multiplier on the wall-clock fallback. Audio-master clock math needs *no* extra multiplication because `played_bytes` counts source-rate input bytes consumed. If you touch the rate code, re-derive that identity before changing it.
- **Snapshots write next to the exe** as `snapshot_YYYYMMDD_HHMMSS.png`. Preserve that path shape unless the user asks for a picker.
- **Resume position** (VLC-style) is loaded on construct, saved on file switch / natural end / destructor. Only positions with `5 s < pos < duration − 10 s` are restored — anything outside that is treated as "start over." Flush to disk immediately after each file switch to survive crashes.

## Interaction patterns to preserve

These are corrections the user has already made — don't regress on them.

- **Seek-drag pauses playback.** The moment `SeekBar` reports `held`, remember `isPlaying()`, call `pause()`, and only resume on release. Otherwise the audio tears and the master clock keeps racing forward against the drag.
- **Double-click for fullscreen must not also pause.** SDL emits `clicks=1` then `clicks=2` on a double-click. The single-click play/pause is deferred by ~280 ms; if a `clicks>=2` arrives during that window it cancels the pending toggle and only fullscreen fires.
- **Overlay auto-hide applies in both windowed and fullscreen when a file is loaded** (splash stays visible). Cursor auto-hides only in fullscreen — in windowed the cursor stays visible so the user can reach title bar / other windows.
- **Only *real* input resets the idle timer.** Do NOT re-add `near_bottom` / `near_top` cursor-region checks or `IsAnyItemHovered()` to the keep-alive condition — a parked cursor over the bar or a button is NOT activity, and the user found the previous behavior distracting. Valid keep-alive signals are: mouse movement (`io.MouseDelta`), `IsAnyItemActive()` (mid-drag), open popup, open playlist panel, and key/wheel/click events (which nudge via `handleEvent`).
- **No green flash on file load.** `SDL_CreateTexture` on a streaming YUV plane leaves memory uninitialized; most GPUs show that as neon green until the first `SDL_UpdateYUVTexture`. `main.cpp` keeps a `has_first_frame` bool that is set only when `updateTexture` returns `true`, reset every time the texture is recreated. The letterbox draw block is gated on it, so the app's dark clear color fills the window during the tiny gap between file open and first decoded frame instead of flashing green. Do NOT remove that gate.
- **`VideoPlayer::play()` must do the full first-play-after-load dance for the audio path, not just Resume.** On some Windows / SDL3 configurations the very first `SDL_ResumeAudioStreamDevice` issued after `SDL_OpenAudioDeviceStream` + `SDL_PauseAudioStreamDevice` in `loadFile` is silently coalesced by SDL — the device thread never starts consuming samples, `SDL_GetAudioStreamQueued` stays equal to `audio_pushed_bytes_` forever, the audio-master clock stays pinned at 0, and `updateTexture` can only release the `pts ≈ 0` frame. User symptom on 2026-07-18: Explorer double-click of an mp4 shows the picture stuck on frame 0. Two escape hatches worked: Space + Space (a real Pause + Resume with a full frame between them), and dragging the seek bar (which calls `flushQueues()` → `SDL_ClearAudioStream` + `resetClockLocked()` zeroes `audio_pushed_bytes_`, giving SDL a clean state edge). So `play()` now detects "this is the first play after loadFile" via `audio_pushed_bytes_ > 0 && audio_seek_base_time_ == 0` and, in that case, does the whole dance: `SDL_ClearAudioStream` + reset `audio_pushed_bytes_` to 0 + `SDL_PauseAudioStreamDevice` + `SDL_Delay(2)` + `SDL_ResumeAudioStreamDevice`. The ~50 ms of pre-play-buffered audio is discarded; the resulting minor A/V desync at start self-corrects as the audio-master clock advances. **Never simplify this back to a single Resume.** Subsequent play() calls after a user-initiated pause skip the Clear + Delay because the device is already in a healthy state by then.

## Still-image support

Vela decodes still images (PNG, JPEG, BMP, TIFF, WebP, TARGA, JPEG 2000, JPEG-LS, PCX, PPM/PGM/PBM, QOI) through the exact same FFmpeg video pipeline used for MP4/MKV/etc. — the `image2` demuxer surfaces a one-frame video stream, our decode thread pushes it, and `updateTexture` uploads it to the YUV texture as if it were the first frame of a movie. Two guardrails matter:

- `VideoPlayer::isStillImage()` returns true for the codec IDs above, and the EOF-detection block in `updateTexture` uses that to **skip firing `onEnded`**. Without that skip, a still image would hit EOF the instant its single frame decodes, `handleNaturalEnd` would auto-advance the playlist, and folder-opens of mixed images and videos would blink through every image inside a couple of render frames instead of stopping to display them.
- Animated GIFs are intentionally NOT classified as still images (`AV_CODEC_ID_GIF` is not in the switch). They play through all their frames like a short video; static single-frame GIFs stop at the last frame the same way any short clip does.

The still-image extensions are wired into the same three places as videos: `isVideoFile()` in `UIController.cpp`, the `openFileDialogWin()` filter, and `resources/vela_reg_install.nsh` (OpenWithList + SupportedTypes so Explorer's "Open with -> Vela" surfaces on PNG/JPG/etc. without displacing the default handler).

## Non-goals

- Cross-platform (Linux/macOS) — not until asked.
- Skinning / theme editor — dark theme is the theme.
- Network streaming / codecs beyond what FFmpeg gives us for free.
- EQ, video filters — out of scope until the user requests them.
- Pitch-preserving speed change (would need SoundTouch/rubberband). Current chipmunk-audio approach is deliberate.
- Styled ASS/SSA subtitles, PGS bitmap subtitles, embedded MKV subtitle streams — external SRT / SMI is enough for the Korean-market case we're targeting today.

## When editing UI

- Test at 640×400 *and* at typical windowed size (1280×720) *and* in fullscreen. All three must look right.
- `overlay_alpha_` fades everything in fullscreen *and* windowed idle; don't draw permanent chrome that ignores it.
- The `Style::Fade(color, alpha)` helper is how per-frame alpha is applied to draw-list primitives; ImGui widgets get `PushStyleVar(ImGuiStyleVar_Alpha, alpha)`.
- When speed ≠ 1.0, a small `1.50x` badge appears left of the time text in the bottom overlay. Keep it — it's a subtle indicator that the user is off-rate.

## Story so far (evolution log)

Roughly chronological, so future sessions understand *why* the code looks the way it does. Add to this list, don't rewrite it.

1. **Foundations** — SDL3 + ImGui + FFmpeg pipeline, decode thread, YUV texture upload, letterbox, dark theme, hand-drawn vector icons, snapshots (`Ctrl+S`), playlist panel, aspect modes, recent-files splash.
2. **Immersive UX pass** —
    - Drop-to-play (was drop-to-enqueue).
    - Seek-drag pauses playback until release, then resumes.
    - Double-click-to-fullscreen no longer double-fires with single-click play/pause (deferred 280 ms).
    - Overlay auto-hide unified across windowed + fullscreen (3 s idle); fade-in snappier than fade-out; cursor hides only in fullscreen.
    - Space and other shortcuts no longer stolen by focused ImGui buttons (`NavEnableKeyboard` removed).
    - Key-repeat whitelisted — only seek/volume nudges auto-repeat on hold.
3. **Standard-player features** —
    - Mouse wheel over video = volume (±5 %, auto-unmute).
    - Playback speed `[ ] \` with a live `Nx` badge; audio via `SDL_SetAudioStreamFrequencyRatio` (pitch changes — accepted tradeoff).
    - Home/End jump; End lands 1 s before EOF to avoid instant natural-end.
    - VLC-style resume position (`vela.resume` next to exe, MRU, safe window `5 s < pos < dur − 10 s`).
4. **Debug-vs-release polish** —
    - Release links as `WINDOWS` subsystem (no console); Debug keeps console.
    - Seek OSDs (`± 5 s`, `± 30 s`, frame step) gated behind `#ifndef NDEBUG` so they don't spam Release viewing.
5. **Shippable packaging** —
    - Multi-size ICO generated by `tools/generate_icon.ps1` (dark rounded square + blue accent + play triangle).
    - `resources/app.rc` embeds the icon + version info.
    - `main.cpp` sets the SDL window's HWND icon via `LoadImageW` + `WM_SETICON`.
    - CPack ZIP + NSIS installer; all runtime DLLs (SDL3, FFmpeg, VC++ redist) bundled; installer registers Start-Menu / Desktop shortcuts and an uninstaller.
6. **Competitive-with-PotPlayer slice** — the "if a Korean user opens this, do they see something they recognise?" bar:
    - Volume boost to 200 %.
    - A-B segment loop (`A` cycles: set A / set B / clear). Green/orange markers plus shaded span on the seek bar.
    - External subtitles: auto-loads `basename.srt` / `basename.smi` next to the video. Full parsers with CRLF / BOM / dot-ms tolerance for SRT; case-insensitive tag handling, `<br>` → newline, entity decoding, per-class filter, and empty-body-as-clear semantics for SMI. Encoding is UTF-8-first with a CP949 fallback for legacy Korean files. Rendered with outlined text via `AddText` at ~34 px; Malgun Gothic merged into the font atlas for hangul. `S` toggles the overlay.
    - Installer registers a `FriendlyAppName` under `HKCR\Applications\Vela.exe` and adds `OpenWithList` entries for every video extension in the file-dialog filter, so Explorer's right-click "Open with -> Vela" surfaces without forcing itself as the default handler.
7. **Testing** — doctest fetched as a single-header via `FetchContent(URL)`, built into `vela_tests`. Covers UTF-8 validation, both subtitle parsers, and their tricky edge cases (BOMs, CRLF, dot-ms timestamps, out-of-order cues, SMI class filtering, entity decoding, garbage input). 21 cases / 61 assertions.
8. **Rebrand + commercial polish (name = Vela)** — big VELA wordmark on the splash with an accent-gradient underline and a "Focused, keyboard-first video playback" tagline; `?` / F1 opens a modal Keyboard Shortcuts panel (grouped by Playback / Seek / Audio / Speed / Files & tracks / View); Settings menu gains "Keyboard Shortcuts" and "About Vela" entries; failed `loadFile` now surfaces a "Cannot open  <basename>" OSD and resets the window title instead of silently doing nothing. Window title format is `filename  -  Vela` (VLC-style, file first).
9. **A/V sync offset** — SDL doesn't expose the audio device's own output-buffer latency, so on some hardware the audio-master clock is systematically ahead of what the user hears and video floats slightly ahead of audio. Rather than guess at the device buffer depth (fragile, driver-specific), we added the same escape hatch every commercial player has: a manual A/V sync offset. `Ctrl+Left / Right` nudge in 30 ms steps, `Ctrl+0` resets. See the A/V sync section under "Interaction patterns to preserve" for the invariant.
10. **Hardware video decode (default on)** — `VideoPlayer::tryInitHwDecode` attempts D3D11VA first, then DXVA2. On success, `avcodec_open2` runs with `hw_device_ctx` attached and our `get_format` trampoline (`pickHwPixFormat`, wired via `codec_ctx->opaque = this`) pins the pipeline to the HW pixel format. Decoded frames come back as HW-backed surfaces; the decode loop's `materializeYUV` helper copies them to a CPU frame via `av_hwframe_transfer_data` and then feeds `sws_getCachedContext` (which handles the format change - typically NV12 - transparently). If any HW step fails, the codec context is reopened as software and playback continues unbroken. On a successful HW load the user gets a short `HW decode  D3D11VA` OSD toast. `isHardwareDecoding()` / `getHardwareDecoderName()` on `VideoPlayer` expose the current state for UI. **Never remove the SW fallback path** - not all codecs / profiles are supported by every GPU, and silently refusing to play is worse than doing a CPU decode.
11. **Release-ready commercial slice** — the "if this shipped tomorrow, would a first-time user find the standard features?" bar:
    - **Playlist controls.** `Playlist` now owns a `RepeatMode` enum (None / One / All) and a `shuffle` flag with an internal `shuffle_order_` permutation. L cycles repeat, H toggles shuffle. `handleNaturalEnd` branches: repeat-One = seek 0 & replay, hasNext = advance, repeat-All + wrap = `reshuffle()` + `wrapToStart()` + load. `addBulk()` exists because Open Folder rebuilds shuffle order per-item otherwise — O(N²).
    - **Open Folder.** Ctrl+Shift+O uses `GetOpenFileNameA` as a folder hint (user picks any file in the target folder), then `FindFirstFileA` scans siblings and enqueues everything matching `isVideoFile()`.
    - **CLI multi-file.** `main.cpp` walks all `argv[1..]` and hands the vector to `ui.enqueuePaths` — dropping a directory of files onto `Vela.exe` "just works."
    - **M3U save/load.** Settings menu writes `#EXTM3U` + one absolute path per line; loader ignores blank/comment lines, calls `playlist_.clear()` then `enqueuePaths(paths, true)`.
    - **Video rotation.** `RotationMode` enum on UIController; Ctrl+R cycles. Render path in `main.cpp` swaps effective aspect for 90/270 and transposes the pre-rotation dst so `SDL_RenderTextureRotated` produces a correctly letterboxed image. Stretch + rot90 fills the full window.
    - **Subtitle QoL.** `SubtitleSize` enum (S/M/L/XL) picked by `subtitleBasePx(W)`, cycled by G. `subtitle_offset_` shifts the lookup timestamp; Ctrl+Shift+Left/Right nudge 100 ms, Ctrl+Shift+0 resets.
    - **Always on top.** T toggles `SDL_SetWindowAlwaysOnTop`; menu item mirrors state; persists.
    - **Persistent settings.** `vela.config` next to the exe stores volume, muted, subtitle size/offset/enabled, always-on-top, repeat mode, shuffle, A/V sync offset, aspect, rotation. Loaded in ctor after `loadResumeIndex`, saved unconditionally in dtor. `markConfigDirty()` exists but current strategy is "save on quit" — good enough given the resume-index flush handles the crash-recovery case for what actually matters (playhead).
12. **Green-flash fix + info modal + playlist polish (post-P0 iteration)** —
    - `SDL_CreateTexture` streaming YUV is uninitialized; on many GPUs it appears neon green until the first `SDL_UpdateYUVTexture`. `main.cpp` now gates the video draw on `has_first_frame`, set only when `updateTexture` returns true. Reset every time the texture is recreated (new file, resolution change).
    - `VideoPlayer` exposes info accessors: `getVideoCodecName`, `getAudioCodecName`, `getVideoFrameRate` (prefers `r_frame_rate`, falls back to `avg_frame_rate`), `getFileBitrate`, `getAudioSampleRate`, `getAudioChannels`. UIController's `drawInfoPanel` renders them as a Media Info modal on I. Never assume a stream exists in the accessors — each returns "" / 0 when absent so the UI code stays branch-light.
    - Playlist panel now has per-row `^` / `v` / `x` buttons and a header Clear button. Mutations (`moveUp` / `moveDown` / `remove`) are collected during the draw loop and applied *after* the child window closes; if you flip that back to in-loop mutation, ImGui IDs shift mid-frame and the click that just fired gets misinterpreted on the next hover.
    - Standard-player shortcut sugar: plain N advances the playlist (in addition to Ctrl+N); F11 toggles fullscreen (in addition to F / Enter).
13. **Release-candidate polish (v1.0)** — final pre-ship pass, driven by a parallel design + PM audit:
    - **Version single-source-of-truth** via `Version.hpp` (`VELA_VERSION "1.0.0"`). CMake `project(...)` bumped in sync. About panel + splash footer both read the macro.
    - **Sidecar persistence.** `vela.recent` (recent-files list, cap 12) and `vela.session.m3u` (last-session playlist, restored without auto-play) sit next to the exe alongside `vela.config` and `vela.resume`.
    - **Broken-playlist recovery.** `loadPath` returns `bool`. `playNextInPlaylist` / `handleNaturalEnd` walk forward skipping items that fail to open, bounded by `playlist_.size()` so an all-broken queue stops.
    - **Stalled-open watchdog.** `VideoPlayer::loadFile` installs an `AVIOInterruptCB` with a 5 s wall-clock deadline (`openInterruptCb`). Disarmed (`open_deadline_ = 0`) after `avformat_find_stream_info` succeeds so it never fires during steady-state reads.
    - **Layout survives 640x400.** Splash "RECENT" half-column is `min(200, W*0.42)`; playlist panel width `= clamp(260, W-300, 340)`; row `^ v` reorder buttons hide when panel narrows below 300 px.
    - **Drag-drop of a folder** enqueues its videos (same expansion as CLI multi-arg). `enqueuePaths` inline-expands any directory argument via `FindFirstFileA` + `isVideoFile()`.
    - **Polish:** playback-rate → `Style::Badge` pill; A/B loop labels moved BELOW the seek track; playlist Clear button disabled when empty; row hover shows filename tooltip.
    - **Splash "?  or  F1"** hint row surfaces the shortcut modal.
    - **About panel** gains "Press Esc to close" line and reads its version from `VELA_VERSION`.

If you're onboarding into a new session: skim the story, glance at `Interaction patterns to preserve`, run the exe once, run `vela_tests` once. Everything else you can read from the code.
