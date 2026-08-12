# Banana Player - Work History

Session-oriented development log. The goal of this file is that anyone (or any AI
assistant in a fresh session) can read it top-to-bottom and reconstruct enough
context to continue the project from any machine.

Read alongside:

- [`CLAUDE.md`](../CLAUDE.md) - project charter, design principles, and the
  "landmines list" of interaction patterns to preserve. Load-bearing for any code
  edit.
- [`CHANGELOG.md`](../CHANGELOG.md) - per-release "what shipped" notes.
- [`docs/FEATURES.md`](FEATURES.md) - exhaustive user-facing feature catalog.

The entries below are grouped by version milestone. Each entry has the same shape:

- **Date + version tag**
- **What shipped** - the change, briefly.
- **Why it was needed** - the pain that drove the change.
- **Landmines** - regressions we hit while getting here, or invariants that must be
  preserved on any follow-up edit. Any code change touching this area must respect
  the landmine or re-derive why it no longer applies.
- **Related memory** - keys under
  `C:\Users\ysj12\.claude\projects\C--work-Player\memory\` for AI sessions to load.

---

## Where we are now

- **Current version:** `1.4.0` (see [`Version.hpp`](../Version.hpp)).
- **Branch:** `master` on the local repo. Remote: `origin -> https://github.com/2uropa2/BananaPlayer.git`.
- **Latest commit (as of this file):** `Three-thread decode pipeline + YUV range toggle + Shift+N normalizer`.
  This is post-1.4.0 work that has not yet been cut as `1.4.1` or `1.5.0`. Bump
  `Version.hpp` + `CMakeLists.txt` + `resources/app.rc` in lockstep before the next
  release; see [`tools/bump_version.ps1`](../tools/bump_version.ps1).
- **Build health:** Release + Debug both build clean under MSVC 2022 with vcpkg
  SDL3 + FFmpeg + imgui. `banana_tests.exe` passes 21 cases / 61 assertions.
- **Immediate open items:** none blocking. Next natural slice would be a `1.4.1`
  cut that folds in the three-thread pipeline + YUV range toggle + `Shift+N`
  hotkey, plus any follow-up fixes surfaced by real-world playback of the
  problem files that motivated those changes (Twitter mp4s, mov chunk-interleave
  files, broadcast-range rips).

---

## Sessions / milestones

### Post-1.4.0 - Three-thread decode pipeline + YUV range toggle + Shift+N normalizer

**Date:** 2026-08-13 (commit not yet released)

**What shipped**

- **Three-thread decode pipeline.** Split the monolithic decode thread into
  demux + video-decode + audio-decode threads with bounded per-stream packet
  queues (`kVideoPktQueueMax = 128`, `kAudioPktQueueMax = 256`). Added
  `video_ctx_mutex_` + `audio_ctx_mutex_` for seek-flush serialization, and a
  sticky `video_flush_pending_` atomic so seek flushes can't deadlock the
  video decode side (the earlier `seek_requested_` check couldn't catch this
  because it was exchange-cleared before the ctx flush began).
- **Post-seek audio skip.** New `audio_skip_to_pts_` atomic. After a seek the
  audio decode thread silently discards any decoded frame whose pts is more
  than 100 ms before the seek target (no push to SDL, no MediaClock nudge).
- **MediaClock seek-pending safety valve.** `media_seek_pending_since_perf_`
  timestamp lets `mediaNowLocked_` unfreeze the clock if the first audio nudge
  takes too long (typical cause: negative-DTS edit list produces a long
  video-only demux run before the first audio packet).
- **Studio YUV range toggle.** Settings -> Video -> "Studio YUV range (16-235)".
  Default is full range (0-255) via `SDL_COLORSPACE_BT709_FULL` because SDL's
  own default (BT.601 LIMITED) washed out modern web / social / phone content.
  Texture rebuilt via `SDL_CreateTextureWithProperties` when the toggle flips;
  a new `texture_generation_` counter in `UIController` is checked by
  `main.cpp` alongside width/height so a colorspace-only change forces a
  texture rebuild.
- **`Shift+N` hotkey** for the loudness normalizer (dynaudnorm) with OSD +
  `banana.config` persistence.

**Why it was needed**

- Twitter / social mp4s with a negative-DTS edit list would freeze on the first
  frame: the demuxer emits several seconds of video packets before the first
  audio packet arrives; with a single decode thread the video queue never
  drained (single-threaded architecture), MediaClock never advanced, and video
  release blocked forever.
- The same class of file also produced a 3-5 s freeze after every seek: `av_seek_frame`
  handed back audio packets 3-5 s before the video keyframe (chunk-interleave
  artifact), MediaClock anchored to that early audio pts, and video release
  was blocked for the whole catch-up window.
- Modern web content is generally full-range 4:2:0; SDL's BT.601 LIMITED default
  clipped highlights and lifted blacks noticeably. Users kept describing it as
  "washed out."
- Loudness normalizer was reachable only through the Settings dialog; adding a
  hotkey matches the pattern set by volume (`M` for mute) and speed (`[ ]`).

**Landmines**

- **Never merge the three threads back into one.** The livelock is baked into
  the ordering guarantees of the demuxer for edit-list files. See
  [[mediaclock-seek-pending-timeout]].
- **The audio-skip window is `kAudioSkipToleranceSec = 0.100`.** Widen it only
  with data - too generous and legitimate audio frames get discarded on precise
  seeks; too tight and mov chunk-interleave files still freeze. See
  [[mov-chunk-interleave-seek]].
- **`media_seek_pending_` is `mutable`** so the `const mediaNowLocked_` can
  promote a frozen clock to a ticking one on timeout. Callers must hold
  `clock_mutex_`. Do not add non-const overloads chasing this thread.
- **YUV range toggle bumps `texture_generation_`.** The old texture-create
  gate looked only at width/height; a colorspace-only change would have
  silently been ignored until the next resolution change. If any future
  Settings item also affects texture creation properties, bump the same
  counter.
- **`Shift+N` scancode check** must come BEFORE the plain-`N` next-in-playlist
  branch and must not fire for `Ctrl+N` / `Alt+N` (both are already meaningful).
  The plain-`N` branch now explicitly excludes `SDL_KMOD_SHIFT`.

**Related memory**

- [[feedback_mov_chunk_interleave_seek]] - the mov file class this addresses.
- [[feedback_mediaclock_seek_pending_timeout]] - the three-thread + timeout
  investigation.
- [[feedback_avio_buffer_and_mediaclock_lineage]] - "when tuning knobs fail,
  look for a bias in the base-position math before adding more knobs".

---

### 1.4.0 - MediaClock overhaul + Settings dialog

**Date:** 2026-08-02

**What shipped**

- **MediaClock architecture.** Replaced the audio-master clock.
  `VideoPlayer::mediaNowLocked_` returns wall-clock anchored time
  (`anchor_pts + elapsed*rate`); `mediaNudgeLocked_(heard)` pulls the anchor
  5 % toward "what the user is hearing right now" on every audio push
  (dead zone < 5 ms, hard-snap > 50 ms). Video release logic reads MediaClock
  via `getMasterClockLocked` / `getCurrentTime` and gets a smooth number that
  no longer inherits SDL's audio-chunk jitter.
- **Post-seek MediaClock freeze.** `media_seek_pending_` freezes the clock
  immediately after `resetClockLocked` and thaws on the first audio nudge
  (which hard-anchors to the true heard pts). Prevents "video fast-forwards
  for 1-5 s after every seek."
- **First-audio-frame pts capture.** `audio_base_needs_capture_` reseats
  `audio_seek_base_time_` to the decoder's actual first-frame pts. Fixes
  per-file lip-sync bias caused by AAC encoder-delay padding (~43 ms),
  MP4 `start_time` offsets, MKV audio track offsets.
- **Embedded subtitle auto-activation** on load: Korean text -> any text ->
  Korean bitmap -> any bitmap. Manual mid-play activation does a 2 s self-seek
  so the decoder catches cues that span the activation moment.
- **Loudness normalize** via `dynaudnorm` (zero-lookahead - `loudnorm` proper
  would break real-time sync with its ~3 s lookahead).
- **Settings dialog.** Six tabs (Playback / Video / Audio / Subtitles /
  Snapshots / Behavior) opened by the gear icon or `Ctrl+,`. Pulled 11 items
  out of the main menu to declutter.

**Why it was needed**

- The audio-master clock inherited SDL's chunked-processing jitter, producing
  visible video micro-stutter on files with tight A/V dependencies.
- Sync bias on many files was systematic: audio consistently late by tens of
  ms even without any user-perceived cause. Root cause turned out to be
  `audio_seek_base_time_` being seeded from the seek TARGET, not the decoder's
  first-frame pts.
- Embedded MKV / MP4 subtitle tracks previously required manual activation
  through the menu; most users didn't discover them.
- The main menu had grown to ~20 items and users struggled to find preference
  toggles buried among per-file actions.

**Landmines**

- **Do NOT re-add auto-measured device-buffer or `video_display_latency_ms_`
  compensation knobs.** Both were tried, both were rolled back. They papered
  over the base-pts bias but were wrong for other reasons. See
  [[feedback_avio_buffer_and_mediaclock_lineage]].
- **Use `dynaudnorm`, not `loudnorm`, for the real-time path.** `loudnorm`
  has ~3 s of lookahead latency that would silently break A/V sync.
- **Gear-icon-click semantics.** Left-click opens Settings dialog; right-click
  opens the main menu. Users have both surfaces in muscle memory now.

**Related memory**

- [[project_vela_1_4_0_release]] - full release note.
- [[feedback_avio_buffer_and_mediaclock_lineage]].

---

### 1.3.0 - GIF export + stats + welcome banner + auto-next countdown

**Date:** 2026-08-01

**What shipped**

- **A-B segment GIF export** via a `palettegen` + `paletteuse` avfilter chain
  (`VideoPlayer::exportSegmentAsGif`). 15 fps / 480 px defaults. Capped at 20 s
  source length to bound RAM.
- **Playback statistics** (`banana.stats`): total watched seconds + session
  count per file. Info panel shows "Watched: 2h 15m x 3 sessions". Tracked
  in `stats_accum_this_session_`, dt-clamped to guard against OS sleep.
- **First-run welcome banner** with four essential tips. Detected by absence
  of `banana.config`; `saveConfig` unconditionally writes `first_run_done=1`
  so the banner appears at most once (or until the config file is deleted).
- **Auto-next countdown**: natural playlist end shows a `Next in 5...`
  overlay with `Play now` / `Cancel` buttons before advancing. Off / 3 / 5 /
  10 s in Settings; 5 s default. Cancelled by any explicit `loadPath`.

**Why it was needed**

- Users wanted to share short highlights from playback without leaving the
  app to run a separate tool.
- The Info panel had no "how much have I actually watched this?" metric.
- First-run experience was blank - no orientation for new users.
- Auto-advance on natural end had bitten users who left the app running
  during dinner and returned to a queue several hours ahead of where they
  paused.

**Landmines**

- **GIF export length cap is deliberate.** `palettegen` buffers frames in
  memory; a full-movie GIF would OOM. If the cap is lifted, switch to a
  chunked pipeline first.
- **Session stats dt-clamp is `2.0 s`.** OS sleep or debugger break can
  otherwise inflate watched-time by hours in one tick.

---

### 1.2.0 - Domestic (Korean) top-10 slice + stability pass

**Date:** 2026-08-01

**What shipped**

Driven by DCInside / Clien / Ruliweb / Coolenjoy / PotPlayer-cafe threads.
Signal was heavy on subtitle workflow (dual-subs, color/font/outline, online
download, in-line search) plus PotPlayer-parity features (burst screenshot,
segment save):

- **Dual subtitles** (`subtitles_secondary_` track, primary above secondary
  at 85% size).
- **Subtitle color preset** (White / Yellow / Cyan / SkyBlue / Pink) and
  **outline thickness** (Thin / Normal / Thick).
- **Subtitle font preset** (Malgun Bold / Gulim / Batang preloaded as
  standalone atlas fonts).
- **Burst screenshots (`Ctrl+Shift+G`).**
- **Subtitle text search (`Ctrl+F`).**
- **Snapshot folder** (empty = default exe folder).
- **Copy / look up current subtitle** (clipboard + Google search).
- **A-B segment export to mp4** via `VideoPlayer::exportSegment` -
  stream-copy from the keyframe at/before A through B, pts normalized to zero.
- **Online subtitle search** - computes OpenSubtitles 64-bit movie hash,
  hands off to `opensubtitles.com` in default browser. No API key, no bundled
  HTTP client.

**Why it was needed**

Feature parity with PotPlayer on the specific things Korean users kept
asking for. This slice was the "make Banana Player a credible domestic
alternative" investment.

**Landmines**

- **`loadPath` per-file resets** now include `subtitles_secondary_`,
  `subtitle_search_last_hit_`, `burst_remaining_`, `video_panning_`.
  Missing any of these would leak state across files.
- **HDR pipeline `hlg_gainlut` null-check** was misleading defensive
  plumbing - `ensureHdrLuts_` guarantees the vector is populated. Removed
  in the stability pass and documented as an invariant.
- **Deinterlace mode change** now self-seeks so the ~8 queued frames get
  dropped and the new mode is visible on the very next displayed frame.

---

### 1.1.0 - HDR + International top-10 slice

**Date:** 2026-07-30 / 31

**What shipped**

- **HDR tone mapping** (PQ + HLG) for SDR-display support. CPU-only
  pipeline in `VideoPlayer::materializeHdrToneMap_`: BT.2020 NCL YUV -> R'G'B'
  -> LUT-driven inverse EOTF (or OETF+OOTF for HLG) -> gamut matrix ->
  BT.709 linear -> fused Hable tone map + BT.709 OETF via a single output LUT.
  Row chunks dispatched to `hardware_concurrency()/2` threads. Cached to
  `banana.hdrlut`.
- **`Ctrl+A`** cycles audio track (backend existed, hotkey added).
- **Pause on window focus loss** (Settings toggle, auto-resumes on refocus).
- **Hardware media keys** (`SDLK_MEDIA_*`).
- **`Shift+P` picture-in-picture** (~480 px wide, bottom-right snap,
  forced always-on-top; restores geometry on second press).
- **Snapshot includes subtitles / HUD** (Settings toggle; captures
  composited back buffer via `SDL_RenderReadPixels`).
- **`J`** cycles external subtitle sidecar (mpv convention).
- **Volume auto-normalize** (Off / Soft / Strong) - peak-based, up-only
  compensation gain clamped [1.0, 4.0].
- **Chapter thumbnails** on the seek bar.
- **Video zoom / pan** (`Ctrl+Wheel` / `Alt+Drag` / `Alt+0`).
- **Deinterlacing (yadif)** - Off (default) / Auto / On.

**Why it was needed**

Feature parity on the international "must-have" axis. HDR in particular
came up over and over because 4K HDR files rendered as washed-out blue-tinted
messes without any tone mapping.

**Landmines**

- **HDR detection routes through a hand-written YUV -> RGB stage.** The
  earlier attempt used `sws_setColorspaceDetails(SWS_CS_BT2020)`, which
  returns garbage or falls back to BT.601 on some FFmpeg builds - that was
  the "everything blue" symptom on the first HDR attempt.
- **Bump `kHdrLutCacheVersion`** when the tone-map math changes so old
  caches are auto-invalidated.
- **yadif is a delay-1 filter.** The pts pushed to the queue must come from
  `filt->best_effort_timestamp`, NOT the current `dec_frame`. Using
  dec_frame's pts drifts A/V by one frame. Default the mode to Off, NOT Auto -
  HandBrake and older encoders routinely tag progressive H.264 as
  `field_order=TT`. See [[feedback_avfilter_pts_and_metadata_trust]].

---

### 1.0.0 - First shipped release

**Date:** 2026-07-22

**What shipped**

The v1.0 slice folded together everything from the foundations pass through
the release-candidate polish. Highlights:

- SDL3 + FFmpeg + Dear ImGui pipeline; decode on its own thread.
- Overlay-first UX: no title bar, no docked panels, 3 s idle auto-hide,
  hero Play/Pause pill, hand-drawn vector icons.
- Standard-player shortcuts (VLC-influenced): Space, F/Enter/F11, arrows,
  PgUp/PgDn, 0-9, `[ ] \`, `A`, `S`, `M`, `T`, `L`, `H`, media keys.
- HW decode (D3D11VA / DXVA2) with a full software fallback.
- External SRT / SMI subs with UTF-8 + CP949 fallback.
- Playlist / repeat / shuffle.
- VLC-style resume position, recent files, session restore.
- Multi-size ICO, `resources/app.rc`, HWND icon injection.
- CPack ZIP + NSIS installer bundling every runtime DLL.
- doctest suite for the subtitle parsers.

**Why it was needed**

The v1.0 milestone was the point at which the experiment stopped being a
"can we?" and became "here's a real player." All later versions build on
this baseline.

**Landmines**

- **Do NOT set `ImGuiConfigFlags_NavEnableKeyboard`.** Clicked overlay
  buttons retain focus and steal Space / Enter, so pressing Space
  re-triggers whichever button was last clicked (e.g. Snapshot instead of
  pause). All shortcuts are handled directly in `UIController::handleEvent`.
- **Hold-to-repeat is a whitelist.** Only `Left` / `Right` (without Ctrl),
  `PgUp` / `PgDn`, `Up` / `Down` act on `event.key.repeat`. Everything else
  fires once per press. Ctrl combos are explicitly excluded because
  holding `Ctrl+Left` would spam 30 ms A/V sync nudges at OS repeat rate.
- **Seek-drag pauses playback.** The moment `SeekBar` reports `held`,
  remember `isPlaying()`, call `pause()`, resume on release. Otherwise the
  audio tears and the master clock keeps racing against the drag.
- **Double-click for fullscreen must not also pause.** Current strategy is
  "toggle immediately, undo on double-click" (`click_toggle_undoable_`).
  Never re-introduce a deferred timer here - the whole point of this
  rewrite was to kill perceived click latency.
- **`VideoPlayer::play()` first-play-after-load dance.** On some Windows /
  SDL3 configurations the very first `SDL_ResumeAudioStreamDevice` after
  `SDL_OpenAudioDeviceStream` + `SDL_PauseAudioStreamDevice` is silently
  coalesced. `play()` detects "this is the first play after loadFile" and
  does the full Clear + Pause + Delay + Resume dance. Never simplify this
  back to a single Resume.
- **No green-flash at file open.** `SDL_CreateTexture` on a streaming YUV
  plane leaves memory uninitialized; most GPUs show that as neon green
  until the first `SDL_UpdateYUVTexture`. `main.cpp` keeps a
  `has_first_frame` bool that is set only when `updateTexture` returns true.
  Do NOT remove that gate.

**Related memory**

- [[project_vela_release_slice]] - full 1.0.0 release note (name = Vela
  at the time).
- [[feedback_player_no_forced_aspect]] - always fit, never stretch.
- [[feedback_player_cpack_nsis_landmine]] - the NSIS quote-escaping gotcha.

---

## Post-1.0 chronology (compressed)

Full detail lives in [`CHANGELOG.md`](../CHANGELOG.md) and in the "Story so far"
section of [`CLAUDE.md`](../CLAUDE.md). The short version:

- **2026-07-22:** 1.0.0 shipped.
- **2026-07-30/31:** 1.1.0 (HDR + international top-10).
- **2026-08-01:** 1.2.0 (Korean top-10 + stability pass) and 1.3.0 (GIF export,
  stats, welcome banner, auto-next countdown) in the same day.
- **2026-08-02:** 1.4.0 (MediaClock, embedded subs, dynaudnorm, Settings dialog).
- **2026-08-04:** Vela -> Banana Player rename. Every internal identifier
  swapped (exe, install dir, CMake target, package name, registry keys,
  sidecar filenames, source macros). Migration hook: any `vela.*` sidecar
  next to the exe on first launch is renamed to `banana.*`.
- **2026-08-13:** Three-thread decode pipeline + YUV range toggle +
  `Shift+N` normalizer (this file's "Where we are now" entry).

---

## Pre-1.0 chronology (compressed)

For fresh AI sessions, the roughly-chronological arc that got us to 1.0:

1. **Foundations** - SDL3 + ImGui + FFmpeg pipeline, decode thread, YUV texture
   upload, letterbox, dark theme, hand-drawn vector icons, snapshots (`Ctrl+S`),
   playlist panel, aspect modes, recent-files splash.
2. **Immersive UX pass** - drop-to-play, seek-drag pauses playback, double-click
   fullscreen doesn't double-fire, unified overlay auto-hide, `NavEnableKeyboard`
   removed, hold-to-repeat whitelisted.
3. **Standard-player features** - mouse wheel volume, playback speed `[ ] \`,
   Home/End, VLC-style resume position.
4. **Debug-vs-release polish** - `WINDOWS` subsystem for release, seek OSDs
   gated behind `#ifndef NDEBUG`.
5. **Shippable packaging** - multi-size ICO, `app.rc`, HWND icon injection,
   CPack NSIS + ZIP bundling every runtime DLL.
6. **Competitive-with-PotPlayer slice** - volume boost to 200 %, A-B segment
   loop, external SRT / SMI with Korean encoding fallback.
7. **Testing** - doctest suite for the subtitle parsers.
8. **First rebrand pass** (product name = Vela at the time) - big wordmark
   splash, `?` / F1 shortcuts modal, About panel, failed-load OSD.
9. **A/V sync offset** - `Ctrl+Left / Right` +/- 30 ms, `Ctrl+0` reset.
10. **Hardware video decode** (default on) - D3D11VA first, then DXVA2,
    with a full software fallback on any failure.
11. **Release-ready commercial slice** - playlist controls, Open Folder,
    CLI multi-file, M3U save/load, video rotation, subtitle QoL, always on
    top, persistent settings.
12. **Green-flash fix + info modal + playlist polish.**
13. **Release-candidate polish (v1.0)** - version single-source-of-truth,
    sidecar persistence, broken-playlist recovery, stalled-open watchdog,
    layout survives `640 x 400`, drag-drop folder expansion, splash "? or F1"
    hint.

---

## How to pick up in a fresh session

If you're an AI assistant loading this file for the first time:

1. **Read [`CLAUDE.md`](../CLAUDE.md) in full.** Its "Story so far",
   "Design principles", "Interaction patterns to preserve", and
   "Non-goals" sections are load-bearing for every code change.
2. **Read the "Where we are now" section at the top of this file** for
   the current branch state and open items.
3. **Read the most recent milestone entry above** (start with "Post-1.4.0"
   and walk backward as needed) to understand what shipped in each of
   the last few sessions and what landmines those changes left behind.
4. **Load the memory keys** referenced under "Related memory" for each
   milestone you touch.
5. **Skim [`CHANGELOG.md`](../CHANGELOG.md)** for the terse "what shipped"
   view.
6. **Run the app once** (`build\Release\BananaPlayer.exe`) with a sample
   video before making any UI-facing change - the user judges by runtime
   behavior, not by diff review.

If you're a human developer coming back after time away:

- The above still applies, plus: `tools\install_local.ps1` refreshes
  `%LOCALAPPDATA%\Programs\BananaPlayer\bin\BananaPlayer.exe` from the
  latest Release build so you can retest a change without touching the
  installer.

---

## Update discipline

Add a new session entry to this file when:

- You ship a version bump.
- You land a change that leaves a durable landmine (an invariant a future
  session would violate if it didn't know).
- You change a design principle in [`CLAUDE.md`](../CLAUDE.md).

Don't add an entry for:

- Every commit. This file is a decision log, not a git log - `git log`
  is authoritative for commit-level history.
- One-line fixes with no invariant implications.
- Speculative "next step" ideas. Put those in an issue tracker if they
  need tracking; this file records what happened, not what might.
