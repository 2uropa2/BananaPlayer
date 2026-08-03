# Banana Player changelog

All notable changes to Banana Player are recorded here. Semantic versioning: `MAJOR`
bumps for incompatible changes, `MINOR` for backward-compatible features,
`PATCH` for backward-compatible fixes. The version string lives in one
place (`Version.hpp`) and is mirrored into `CMakeLists.txt` and
`resources/app.rc`.

## 1.4.0 - 2026-08-02

Sync overhaul, embedded subtitle auto-activation, loudness normalizer,
and a proper Settings dialog. A/V sync in particular got a full
architectural rewrite that removes the class of "close but not quite"
bugs that manual latency tuning could never fully paper over.

### Added

- **MediaClock architecture**: wall-clock anchored master time source
  independent of any specific renderer's chunking. Video release logic
  now reads a smooth, monotonically-interpolated clock instead of the
  raw audio-played-bytes value that used to bunch frames after every
  SDL chunk. Audio decode thread nudges the anchor toward the true
  heard-position (dead-zoned + rate-limited) so long-term sync stays
  audio-locked without inheriting the chunking jitter.
- **Post-seek MediaClock freeze**: the seek transient no longer lets
  the clock race ahead of what audio can deliver, killing the
  "video fast-forwards for the first 1-5 s after every seek"
  regression that MediaClock v1 rollout briefly introduced.
- **First-audio-frame pts capture**: `audio_seek_base_time_` is
  seeded with the actual decoder-output pts, not the seek target.
  AAC encoder-delay padding (~43 ms), MP4 `start_time` offsets,
  and MKV audio-track offsets no longer bake in a per-file
  constant lip-sync error.
- **Embedded subtitle auto-activation**: MKV / MP4 files with no
  external sidecar auto-select an embedded track on load.
  Priority: Korean text -> any text -> Korean bitmap -> any bitmap.
  Manual mid-play activation also does a tiny 2 s self-seek so the
  decoder catches cues that span the activation moment.
- **Loudness normalize (dynamic)**: dynaudnorm audio filter for
  quiet-dialogue content. Zero-lookahead so it doesn't break A/V
  sync the way loudnorm proper would.
- **Settings dialog** (`Ctrl+,` or gear icon): six-tabbed
  configuration surface (Playback / Video / Audio / Subtitles /
  Snapshots / Behavior). Pulls 11 items out of the main menu that
  used to make it hard to find anything. Gear icon opens the
  dialog directly; the main menu is one right-click away on the
  video surface (or right-click on the gear icon itself).

### Fixed

- **A/V sync drift, "microscopic" variety** — pre-1.4 code computed
  heard-audio position from a seek base set to the seek TARGET
  (usually 0.0), but the decoder's first audio frame is generally
  at some non-zero pts. Captured live now, offset gone.

### Removed

- **Speculative sync fixes that were not the real bug**:
  auto-measured OS device buffer via `SDL_GetAudioDeviceFormat`,
  video-display-latency compensation knob, and the migration hack
  that rewrote old configs' `audio_out_latency_ms=30` to 0. All
  rolled back once base-pts capture proved to be the actual root
  cause.

### Backbone stabilization (release-hardening audit)

Four bugs found and fixed during a pre-ship audit pass:

- **PGS bitmap subtitle memory leak / stale-cue rendering**:
  `sub_bitmap_cues_` was never cleared on track switch or file
  switch - only trimmed at 500 entries. Symptom: PGS cues from
  the previously-selected track continued to render on the new
  track's timeline until the trim fired, plus tens of MB of RGBA
  data leaked per switch. `closeEmbeddedSubtitleDecoder_` now
  clears both text and bitmap cue vectors.
- **HW decode fallback GPU-device leak**: when D3D11VA init
  succeeded but `avcodec_open2` failed and we fell back to SW,
  the orphaned `hw_device_ctx_` reference stayed alive for the
  file's entire lifetime. Fixed to unref alongside the codec
  attachment.
- **Seek race under drag-spam**: `seek_requested_ = false` at
  the end of the decode-thread seek handler could clobber a
  concurrent UI-side re-request. Switched to `exchange(false)`
  at the top of the branch so subsequent requests are preserved
  for the next iteration.
- **First-play dance clobbered pts-capture state**: the
  audio un-stick sequence in `play()` cleared `audio_pushed_bytes_`
  but not `audio_base_needs_capture_` / `media_seek_pending_`,
  leaving MediaClock estimating from stale bytes that never
  reached SDL. Now re-arms both flags after the clear.
- **SMI parser signed overflow (UB) on malformed timestamps**:
  the `Start=` attribute accumulator was `long long` with no
  cap. A broken SMI file with a 20-digit stamp would overflow
  the multiply. Switched to `unsigned long long` with a
  10^15 ms cap - real-world timestamps sit well under this.
- **Loudness filter graph build-failure spin**: if `dynaudnorm`
  couldn't accept the file's sample format, the decode thread
  retried the alloc + build every audio frame. Added a
  `loudness_build_failed_` latch that's cleared on toggle, on
  format change, and on unload / file switch (previously the
  latch would persist across file switches - now it doesn't).
- **Thumbnail cache cross-file contamination**: the 2-second
  bucket key is not file-unique, so an old-file decode that
  finished after an `openFile` clear would land in the cache
  under a bucket the new file's hover could hit - showing the
  old file's frame. Added a generation counter bumped on every
  open / close; stale-generation uploads are discarded at drain.
- **Burst snapshot filename collision**: `timestampString()` had
  1-second resolution, so burst mode at 0.5 s interval could
  produce two snapshots in the same wall-clock second and the
  second one silently overwrote the first. Bumped to millisecond
  precision.
- **Sidecar files vulnerable to crash-during-write truncation**:
  all sidecar writes (banana.resume / .bookmarks / .stats / .config
  / .recent / .session.m3u) went through open("wb") + fclose,
  which truncates the target immediately. A crash or power loss
  during the write would leave an empty or partial file - resume
  positions and stats disappearing on the next launch. Every
  sidecar now writes to `<path>.tmp` first, flushes, then
  `MoveFileEx(REPLACE_EXISTING | WRITE_THROUGH)` renames it into
  place. Windows same-volume rename is atomic so the original
  survives if we crash mid-write.

## 1.3.0 - 2026-08-01

Feature additions built on top of the 1.2.0 base in a single
autonomous session.

### Added

- **GIF export from A-B segment** (Settings > **Save A-B as GIF...**).
  Uses libavfilter's `palettegen` + `paletteuse` for good color
  reproduction. 15 fps / 480 px width defaults; capped at 20 s of
  source to bound palettegen's in-memory frame buffer.
- **Per-file playback statistics** — `banana.stats` sidecar records
  cumulative watched-seconds and session count per file. Info panel
  now shows `Watched: 2h 15m x 3 sessions`. Live-tracking (ticks up
  while playing, ignores pause; sleep-safe via dt clamp).
- **First-run welcome banner** — shown on the splash on the very first
  launch (no `banana.config` on disk). Four essential tips + "Got it"
  dismiss. Suppressed forever once the config is written.
- **Auto-next countdown** (Settings > **Auto-next countdown**: Off / 3
  / 5 / 10 s). On natural end, a small top-right overlay shows a
  `Next in 5...` counter with `Play now` / `Cancel` buttons instead
  of the previous instant advance.

### New Settings items

- Save A-B as GIF... (below Save A-B segment)
- Auto-next countdown (Off / 3 s / 5 s / 10 s)

### New `banana.config` keys

- `first_run_done` (int, 0/1) - set to 1 once the app has ever exited
  normally, suppresses the welcome banner.
- `auto_next_countdown` (int, seconds; 0 = immediate = old behavior).

### New sidecar file

- `banana.stats` next to the exe. Format: `secs<TAB>sessions<TAB>path`
  per line, MRU-ish by session count, capped at 500 entries.

---

## 1.2.0 - 2026-08-01

Two large feature batches landed since 1.0: an international "top-10
missing features" pass (v1.1 slice) and a Korean-community-driven
"top-10" pass (v1.2). Alongside them: HDR tone mapping and an A/V-sync
regression fix.

### Added - Video

- **HDR tone mapping (PQ / HLG -> SDR)**. CPU-only pipeline: BT.2020
  YUV -> RGB (hand-rolled to bypass a broken libswscale BT.2020 path)
  -> LUT-driven inverse EOTF / OETF+OOTF -> BT.2020 to BT.709 gamut
  -> fused Hable filmic + BT.709 OETF via a second LUT. Multi-threaded
  across `hardware_concurrency()/2` rows. Cached LUTs (`banana.hdrlut`
  next to the exe) skip the ~30 ms recompute on every launch.
- **HDR toggle** (Shift+H, Settings menu, and a "HDR" pill button in
  the bottom overlay that only appears while an HDR file is loaded).
  Passthrough mode ships raw values to the display for HDR monitors.
- **Video zoom / pan**. Ctrl+wheel zooms 50 % - 400 %, Alt+drag pans,
  Alt+0 resets. Per-file transient (never persisted).
- **Deinterlacing**. libavfilter `yadif` chain. Settings menu offers
  Off (default) / Auto / On. HW-decoded frames skip the filter (driver
  already deinterlaces). Auto engages on `field_order != PROGRESSIVE`.

### Added - Audio

- **Audio track cycle** (Ctrl+A) - existing menu-only backend gains a
  hotkey.
- **Volume auto-normalization**. Off / Soft (peak -> 0.90) / Strong
  (peak -> 0.98). The decode thread samples the first ~4 seconds of
  playback for a peak magnitude, then multiplies a compensating gain
  (clamped [1.0, 4.0] - never quiets a source).
- **Hardware media keys**. Play/Pause, Stop, Next, Previous route via
  `SDLK_MEDIA_*` when the window has focus.

### Added - Subtitles

- **Dual subtitles** (primary + secondary sidecar rendered together
  for language-learning workflows). Secondary is drawn at 85 % size in
  a slightly dimmer color below the primary line.
- **Cycle sidecar track** (J, mpv convention).
- **Text search** (Ctrl+F) with a floating panel; Enter walks matches
  across primary + secondary.
- **Color preset** (White / Yellow / Cyan / SkyBlue / Pink), **outline
  thickness** (Thin / Normal / Thick), and **font preset** (Default,
  Malgun Gothic Bold, Gulim, Batang).
- **Copy current line to clipboard** and **look up current line in
  browser** (Google search URL).
- **Find subtitles online...** - computes the OpenSubtitles 64-bit
  movie hash and hands off to `opensubtitles.com` in the default
  browser with hash + basename prefilled. No API key needed.

### Added - Snapshots

- **Burst screenshots** (Ctrl+Shift+G). Fires N shots at fixed
  interval; second press mid-burst cancels. Count 5/10/20/50 and
  interval 0.2/0.5/1.0/2.0 s in Settings.
- **Snapshot folder picker** (Settings). Non-empty overrides the
  default `next to BananaPlayer.exe` behavior - useful when the exe is under
  Program Files.
- **Snapshot includes subtitles / HUD** (Settings toggle). Grabs the
  composited SDL back buffer via `SDL_RenderReadPixels` instead of
  the source frame.
- **Save A-B segment to file**. Stream-copies (no re-encode) the
  current A -> B range to a new .mp4 / .mkv. Menu item is enabled
  only when both A and B are set.

### Added - Window / UX

- **Picture-in-picture** (Shift+P). Shrinks the window to ~480 px
  wide, snaps to the bottom-right corner, forces always-on-top. Second
  press restores prior geometry + AoT state.
- **Pause on window focus loss** (Settings toggle). Off by default.
- **Chapter thumbnails**. The seekbar thumbnail popup snaps to the
  nearest chapter tick and prints the chapter title below the image.

### Fixed

- **A/V drift on progressive files** (v1.2, 2026-07-31). The v1.1
  deinterlacing wrapper was pushing frames to the video queue tagged
  with the current `dec_frame`'s pts even when yadif's output was
  actually the previous frame (yadif is a delay-1 filter). Now the
  wrapper reads the pts from the filtered frame. Default deinterlace
  mode also changed from Auto to Off - some encoders (HandBrake, older
  MP4 tools) tag progressive H.264 as `field_order=TT`, which would
  have auto-engaged yadif on content that didn't need it.
- **HDR "everything blue"** (v1.1, 2026-07-30). First implementation
  relied on `sws_getCoefficients(SWS_CS_BT2020)` for the YUV -> RGB
  matrix; the returned table is unreliable across FFmpeg versions
  (garbage / BT.601 fallback on some builds). Rewrote the YUV -> RGB
  stage by hand.

### Changed

- **Deinterlace default** flipped from `Auto` to `Off` (see fix above).
- **Overlay-hide keep-alive** rules unchanged - documented explicitly:
  only real input (mouse motion, key/wheel/click, active drag, open
  popup, open playlist) resets the idle timer.

### Packaging

- Bundles now include `avfilter-11.dll` (new dependency added by the
  deinterlace feature).
- New top-level `LICENSE` (MIT for Banana Player's own code) and
  `LICENSE-3RDPARTY` (attribution for SDL / FFmpeg / ImGui / yt-dlp /
  MSVC redistributable). Both are shipped to `share/` by the installer.
- Installer + portable zip filenames now version-tagged: 
  `BananaPlayer-1.2.0-win64.exe`, `BananaPlayer-1.2.0-win64.zip`.

### New shortcuts summary

| Combo | Action |
|---|---|
| `Ctrl+A` | Cycle audio track |
| `Ctrl+F` | Search subtitle text |
| `Ctrl+Shift+G` | Burst screenshots (or cancel mid-burst) |
| `Ctrl+Wheel` | Video zoom in / out |
| `Alt+Drag` | Pan video (when zoomed) |
| `Alt+0` | Reset zoom / pan |
| `Shift+H` | HDR tone map on / off (HDR files only) |
| `Shift+P` | Picture-in-picture toggle |
| `J` | Cycle external subtitle sidecar |
| Media keys | Play / Pause / Stop / Next / Prev (when window focused) |

### New Settings items

- Volume normalize (Off / Soft / Strong)
- Deinterlace (Off / Auto / On)
- Snapshot folder... (picker) + Reset
- Snapshot includes subtitles / HUD
- Burst snapshots (count + interval sub-menus)
- Pause when window loses focus
- Picture in Picture
- HDR tone map (SDR display)
- Subtitles: Secondary track (dual subs), Color, Outline, Font, Copy /
  Look up current line, Find subtitles online

### New `banana.config` keys

`hdr_tonemap`, `pause_on_focus_loss`, `snapshot_overlays`,
`snapshot_dir`, `burst_count`, `burst_interval`, `subtitle_color`,
`subtitle_outline`, `subtitle_font`, `normalize_mode`,
`deinterlace_mode`. All optional - a config missing any key gets the
new default. Old configs load cleanly.

---

## 1.0.0 - 2026-07-22

First shipped release. See `CLAUDE.md` "Story so far" for the full
feature evolution. Highlights: SDL3 + FFmpeg + Dear ImGui, HW decode
(D3D11VA/DXVA2), external SRT/SMI subs with Korean encoding fallback,
playlist / repeat / shuffle, chapter navigation, bookmarks, playback
history, YouTube URL support via yt-dlp, snapshots (PNG +
clipboard), frame stepping with a ring-buffer history for seek-less
back-step.
