#pragma once

#include <SDL3/SDL.h>
#include "imgui.h"
#include "VideoPlayer.hpp"
#include "Playlist.hpp"
#include "Style.hpp"
#include "Subtitles.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class ThumbnailProvider;
class YoutubeResolver;

enum class RotationMode {
    Deg0,
    Deg90,
    Deg180,
    Deg270,
};

enum class SubtitleSize {
    Small,
    Medium,
    Large,
    XL,
};

// yt-dlp -f selector presets for YouTube playback. All entries are
// single-file selectors because VideoPlayer::loadFile handles one URL;
// a merged video+audio path (needed for true 1080p+ on YouTube) would
// require a two-URL refactor that's out of scope for this slice.
enum class YoutubeQuality {
    Auto,       // "best"                - highest single-file quality
    Q1080,      // "best[height<=1080]"  - cap at 1080
    Q720,       // "best[height<=720]"
    Q480,       // "best[height<=480]"
    AudioOnly,  // "bestaudio"           - audio-only track (no picture)
};

class UIController {
public:
    UIController(VideoPlayer* player, SDL_Window* window, SDL_Renderer* renderer);
    ~UIController();

    void newFrame();
    void render(int windowW, int windowH);
    void handleEvent(const SDL_Event& event);

    bool isFullscreen() const { return fullscreen_; }
    void setFullscreen(bool val);

    // Explicit shutdown hook so main.cpp can tear down SDL-texture-owning
    // subsystems (thumbnail cache) BEFORE SDL_DestroyRenderer runs. The
    // UIController itself lives until end-of-main, whose destructor would
    // otherwise fire after the renderer is gone.
    void shutdownBeforeSdl();

    void openFileDialog();
    // Returns true if the file loaded and playback started; false on failure
    // (a "Cannot open..." OSD is shown either way). Callers that walk the
    // playlist use the return value to skip broken items.
    bool loadPath(const std::string& path);
    void enqueuePaths(const std::vector<std::string>& paths, bool play_first = true);
    // Unified open entry point: routes YouTube URLs through the yt-dlp
    // resolver and everything else through loadPath. Used by pasteAndPlay,
    // the CLI-arg path in main.cpp, and any future URL-open dialog so all
    // three surfaces share the same YouTube branch.
    void openAny(const std::string& path);
    // Case-insensitive URL detection for YouTube pages we know yt-dlp
    // handles - watch, shorts, live, youtu.be short-form, music. Exposed
    // as a static so unit tests could reach it without an SDL context.
    static bool isYoutubeUrl(const std::string& s);

    RotationMode getRotation() const { return rotation_; }
    // YUV range override exposed to main.cpp's texture creation.
    // Auto (default) honors the stream's codecpar color_range: Limited
    // when AVCOL_RANGE_MPEG / UNSPECIFIED (virtually all distribution
    // content), Full when AVCOL_RANGE_JPEG (MJPEG / JPEG stills / some
    // phone camera streams). The forced modes exist as a rescue for
    // mis-tagged files. See member docs.
    enum class YuvRangeMode { Auto = 0, ForceLimited = 1, ForceFull = 2 };
    YuvRangeMode getYuvRangeMode() const { return yuv_range_mode_; }
    // Monotonic counter bumped whenever a texture-affecting Settings
    // value changes. main.cpp compares this against its last-seen
    // value and rebuilds the SDL texture when it moves - the create
    // gate that only looked at width/height would otherwise miss a
    // colorspace-only change.
    unsigned int getTextureGeneration() const { return texture_generation_; }

    std::function<void()> onExitRequested;

private:
    void drawSplash(int W, int H);
    void drawExportProgress(int W, int H);
    ExportJob export_job_;
    // Fills the empty video area when the loaded file is audio-only (.m4a
    // / .mp3 / voice memos): centered filename + "AUDIO ONLY" label on a
    // dark background, so the window doesn't just look like a hung player
    // while audio plays through the normal seek/volume/OSD stack.
    void drawAudioOnlyPlaceholder(int W, int H);
    void drawTopOverlay(int W, int H, float alpha);
    void drawBottomOverlay(int W, int H, float alpha);
    void drawPlaylistPanel(int W, int H, float alpha);
    void drawOsd(int W, int H);
    void drawFlash(int W, int H);
    void drawMainMenuContents();

    void showOsd(const std::string& text, float duration = 1.6f);
    void takeSnapshot();
    // Same as takeSnapshot() but pops a Windows Save As dialog first so
    // the user picks the destination. Bound to Ctrl+S. Cancelling the
    // dialog is a silent no-op. Burst snapshots stay on takeSnapshot()
    // because you can't dialog 10 times in a row.
    void takeSnapshotAskPath();
    void playNextInPlaylist();
    void playPrevInPlaylist();
    void handleNaturalEnd();
    void nudgeVisible();
    void addRecent(const std::string& path);

    VideoPlayer* player_;
    SDL_Window* window_;
    SDL_Renderer* renderer_;

    bool fullscreen_ = false;
    float overlay_alpha_ = 1.0f;
    float overlay_target_ = 1.0f;
    float ui_visible_timer_ = 3.0f;
    // Cursor hides faster than the overlay bar (typical player behavior:
    // pointer disappears after ~1 s so it stops covering the picture, but
    // the overlay stays for its full 3 s so a viewer glancing down still
    // sees the seek bar / play button). Only ticks in fullscreen; windowed
    // keeps the cursor visible unconditionally. `cursor_visible_` tracks
    // our locally-owned state so we don't hammer SDL every frame.
    float cursor_idle_timer_ = 1.0f;
    bool  cursor_visible_ = true;

    struct OsdMsg { std::string text; float remaining; float total; };
    std::vector<OsdMsg> osd_;

    float snapshot_flash_ = 0.0f;

    bool show_playlist_ = false;
    RotationMode rotation_ = RotationMode::Deg0;
    bool always_on_top_ = false;
    // Time display in the bottom bar cycles on click:
    //   0 = "MM:SS  /  MM:SS"       (elapsed / total)
    //   1 = "-MM:SS"                (time remaining)
    //   2 = "MM:SS   ends 21:47"    (elapsed + wall-clock end time)
    // Standard player affordance - PotPlayer, VLC, MPC, YouTube all do
    // some form of this. Persisted in banana.config as `time_display_mode`.
    int time_display_mode_ = 0;
    // Tracks the last playlist index we auto-scrolled the panel to.
    // When playlist_.index() advances (natural end, N key, click)
    // and the panel is open, drawPlaylistPanel calls SetScrollHereY
    // for the new current row so it stays visible. Comparing against
    // this cache avoids the scroll firing every frame.
    size_t last_playlist_scroll_idx_ = (size_t)-1;
    // Playlist filter input. Only shown when the playlist has more
    // than a handful of items - a 4-file queue doesn't need search.
    // Case-insensitive substring match against baseName; empty string
    // shows everything. Not persisted.
    char playlist_filter_buf_[128] = {0};
    // Tracks the last chapter index we OSD'd. When natural playback
    // crosses into a new chapter, the main loop compares current-time
    // against chapters_[] and fires a subtle "Chapter: X" toast. Reset
    // to -1 on every loadPath so the FIRST chapter of a new file also
    // announces (users often skim intro chapters and want to know
    // when the actual content starts).
    int last_chapter_announced_ = -1;
    // HDR tone-map toggle (Shift+H). Default on; users with HDR displays
    // can flip it off to send source values straight through the SDR
    // pipeline without our Hable+709 flattening. Persisted in banana.config.
    bool hdr_tonemap_enabled_ = true;
    // YUV input-range override for SDL YUV texture uploads. Auto is the
    // right default: it reads codecpar->color_range and sets the SDL
    // texture colorspace accordingly (Limited for AVCOL_RANGE_MPEG /
    // UNSPECIFIED, Full for AVCOL_RANGE_JPEG). The earlier design
    // hard-defaulted to Full and lifted blacks on every Limited-range
    // source - WEB-DL / Blu-ray / broadcast - because SDL then mapped
    // Y=16 as ~6 % gray instead of true black. Force* overrides are
    // kept for rescuing mis-tagged files. Change bumps
    // texture_generation_ so main.cpp rebuilds the texture.
    YuvRangeMode yuv_range_mode_ = YuvRangeMode::Auto;
    // Bump this whenever a texture-affecting setting changes so
    // main.cpp's texture-creation gate sees "dims same, but token
    // moved" and rebuilds with the new colorspace property.
    unsigned int texture_generation_ = 0;
    // Audio output latency in milliseconds. Persisted here; passed to
    // VideoPlayer::setAudioOutputLatencyMs on load and on any change.
    // Default 30 matches typical Windows WASAPI shared mode with wired
    // audio; users with Bluetooth headphones or specific DACs may need
    // 60-200 ms; users on exclusive-mode / low-latency setups can go
    // 0-10 ms. Tuned once per system, not per file.
    int audio_out_latency_ms_ = 30;
    // Loudness normalize (dynaudnorm). Off default - engages a filter
    // in the audio decode path. Persists across files.
    bool loudness_normalize_ = true;
    // Voice boost. Cross-forum "explosions loud, dialogue quiet" fix.
    // Off by default so the audio graph stays inert on load.
    bool voice_boost_ = false;
    // Half-res-HDR user preference. Auto is default and turns half-
    // res on for 4K+ HDR (keeps 4K HDR playback smooth without a
    // config trip). Stored here just to persist to banana.config; the
    // actual load-time decision lives in VideoPlayer. Old config key
    // hdr_half_res=0 (from pre-1.4 users) maps to Auto, hdr_half_res=1
    // maps to ForceOn - no explicit-Off value from the old bool, so
    // opt-out is a fresh action for those users.
    int hdr_half_res_mode_ = 0;   // 0 Auto, 1 ForceOn, 2 Off
    // High-res SDR source downscale mode (matches VideoPlayer::HighResDownscaleMode
    // integer values: 0 Off, 1 Auto, 2 ForceOn). Default Auto so 8K
    // sources on typical desktops don't stutter out of the box. See the
    // VideoPlayer::setHighResDownscaleMode doc for the underlying
    // rationale (~150 MB/frame memory bandwidth on the 8K round-trip).
    int hires_downscale_mode_ = 1;   // 0 Off, 1 Auto, 2 ForceOn
    // When true, playback auto-pauses when the window loses focus and
    // auto-resumes when it regains focus. Default off - lots of viewers
    // background their video on purpose while doing something else. The
    // resume-on-focus behavior only fires if we paused ourselves; a
    // manual pause survives the focus event unchanged.
    bool pause_on_focus_loss_ = false;
    bool paused_by_focus_loss_ = false;   // runtime flag, not persisted
    // Latched true once we've shown the "playback stalled" OSD for the
    // current file, so the toast doesn't spam every frame. Reset in
    // loadPath alongside other per-file state.
    bool     decoder_stuck_notified_ = false;
    // Timestamp (SDL_GetTicks) when the decoder-stuck watchdog first
    // fired for this file. We wait ~1.2 s before actually complaining so
    // the "short file + repeat-one + decoder warmup" livelock has time
    // to self-heal once repeat-One is suppressed (the moment the flag
    // is set, handleNaturalEndImmediate stops re-triggering seek(0),
    // the decode thread drains, and frames start flowing). Only true
    // codec failures (HEVC 12-bit against a build without Rext, etc.)
    // sit at 0 frames past the grace window.
    Uint64   decoder_stuck_since_ms_ = 0;
    // Picture-in-picture mode: shrinks the window to a compact size in
    // a screen corner and forces always-on-top. Toggle via Shift+P.
    // The captured geometry / always-on-top state is restored on exit.
    // Not persisted - PiP is a "right now" mode, not a session default.
    bool pip_active_ = false;
    int  pip_saved_x_ = 0, pip_saved_y_ = 0;
    int  pip_saved_w_ = 0, pip_saved_h_ = 0;
    bool pip_saved_always_on_top_ = false;
    bool pip_was_maximized_ = false;
    void togglePipMode();
    // Snapshot capture mode. Default (false) writes the source frame
    // straight from the decoder - no overlay, no subs, no HUD. When
    // true, Ctrl+S instead reads the composited SDL back buffer, so the
    // PNG includes whatever is on screen (subtitles + HUD + OSD). Wait
    // ~3 s for the overlays to fade before triggering if a "video + subs"
    // capture is what you want.
    bool snapshot_include_overlays_ = false;
    // Per-file playback statistics. `banana.stats` sidecar records total
    // watched seconds + open-count per file, MRU-ordered, capped at 500
    // to bound the sidecar. `stats_accum_this_session_` accumulates
    // real playing time (not wall time - pauses don't count) and is
    // committed to the map on file switch / quit.
    struct PlaybackStats {
        double seconds  = 0.0;
        int    sessions = 0;
    };
    std::unordered_map<std::string, PlaybackStats> stats_;
    double stats_accum_this_session_ = 0.0;
    bool   stats_dirty_ = false;
    void   loadStatsIndex();
    void   writeStatsIndex();
    void   commitCurrentStats();
    PlaybackStats getStatsFor(const std::string& path) const;
    // Custom folder for Ctrl+S snapshots and Ctrl+Shift+G bursts.
    // Empty = use exe-adjacent (the historical default). Persisted in
    // banana.config. Useful when the exe lives under C:\Program Files
    // (no write access) or the user wants everything landing in Pictures.
    std::string snapshot_dir_;
    void openSnapshotFolderDialog();
    // Save-as dialog + stream-copy of the current A-B segment to a new
    // media file. No-op with an OSD when A-B markers aren't both set.
    void exportABSegmentToFile();
    void exportABSegmentAsGif();
    // Copy the currently displayed subtitle line to the clipboard as
    // CF_UNICODETEXT. Silent no-op when no cue is on screen. Handy
    // for language-learning workflows where the user wants to keep
    // difficult lines for later review.
    void copyCurrentSubtitleToClipboard();
    // Open the default browser on a Google search URL prefilled with
    // the current subtitle text. Same guard as copy - only fires when
    // a cue is on screen.
    void lookupCurrentSubtitleInBrowser();
    // Open OpenSubtitles.com search in the default browser, prefilled
    // with the current file's basename AND the OpenSubtitles movie hash
    // when the source is a plain local file. Handles the 90% case
    // (find + download the right subtitle) without needing an API key,
    // rate limits, or a bundled HTTP client. The user drags the .srt
    // back onto Banana Player to load - our existing drop handler does the rest.
    void searchSubtitlesOnline();
    // Burst-screenshot state (Ctrl+Shift+G). Fires N snapshots at a
    // fixed interval so a user can build a GIF or manual reaction pack
    // without repeatedly hitting Ctrl+S. `remaining` counts down each
    // shot; `next_at` is seconds until the next snapshot (initialised
    // to 0 so the first shot fires the same frame the user triggered).
    int   burst_count_    = 10;     // shots per trigger
    float burst_interval_ = 0.5f;   // seconds between shots
    int   burst_remaining_ = 0;
    float burst_next_at_   = 0.0f;
    void triggerBurstSnapshot();
    // Subtitle text search (Ctrl+F). A small floating panel accepts a
    // case-insensitive substring; Enter jumps to the next matching cue
    // in `subtitles_` (wrap on end). last_hit_idx_ tracks the previous
    // match so repeated Enter walks through duplicates.
    bool show_subtitle_search_ = false;
    char subtitle_search_query_[128] = "";
    int  subtitle_search_last_hit_ = -1;
    bool subtitle_search_focus_next_ = false;
    // First-run onboarding banner. `first_run_hint_pending_` starts true
    // if no banana.config was found at ctor time, then flips false and
    // gets persisted to `first_run_done=1` after either (a) the user
    // dismisses it, or (b) a file has been playing for a few seconds.
    // Shown only while nothing is loaded (splash screen).
    bool first_run_hint_pending_ = false;
    void drawFirstRunHint(int W, int H);
    // Countdown before auto-advancing to the next playlist item on
    // natural end. 0 seconds disables (immediate advance = old behavior).
    // Default 5 s. Persisted as `auto_next_countdown`. `..._active_`
    // tracks whether a countdown is currently displayed; `..._secs_`
    // is the live remaining seconds.
    int   auto_next_countdown_ = 5;
    bool  auto_next_active_    = false;
    float auto_next_secs_      = 0.0f;
    // Single-instance mode. When true, double-clicking a file (or
    // launching BananaPlayer.exe with a path) while an existing Banana Player window
    // is open forwards the path to that instance via a named pipe
    // and the new process exits without showing a second window.
    // Default false = always launch a new window (historical behavior).
    // The actual mutex + pipe plumbing lives in main.cpp; this bool is
    // just the persisted user preference read once at process start.
    bool single_instance_mode_ = false;
    void  handleNaturalEndImmediate();
    void  drawAutoNextOverlay(int W, int H);
    void  cancelAutoNext();
    void openSubtitleSearch();
    void drawSubtitleSearchPanel(int W, int H);
    void runSubtitleSearchStep(int direction);
    // Video zoom / pan. Zoom is a multiplier applied to the letterbox
    // dst rect in main.cpp. Pan is in NORMALIZED window units (so the
    // offset scales with the window - +/- 0.5 == half a window in that
    // axis). Per-file transients; reset in loadPath.
    float video_zoom_  = 1.0f;
    float video_pan_x_ = 0.0f;
    float video_pan_y_ = 0.0f;
    // Panning is triggered by Alt+left-drag. The button-down handler
    // captures Alt and starts tracking; motion accumulates delta into
    // video_pan_x_/y_; button-up ends it. The play-pause single-click
    // handler is suppressed while a pan is in progress.
    bool  video_panning_    = false;
    float video_pan_start_x_ = 0.0f;
    float video_pan_start_y_ = 0.0f;
    float video_pan_orig_x_  = 0.0f;
    float video_pan_orig_y_  = 0.0f;
    // Click-and-drag window move. Plain left-button-down over the video
    // area records the anchor here; the first MOUSE_MOTION event whose
    // delta from the anchor exceeds ~4 px flips `window_dragging_`
    // true, cancels the pending play/pause click, and starts moving
    // the OS window with SDL_SetWindowPosition. Below the threshold
    // it stays a click and the existing pause/fullscreen path fires.
    bool  window_drag_pending_ = false;
    bool  window_dragging_     = false;
    float window_drag_mouse_x_ = 0.0f;
    float window_drag_mouse_y_ = 0.0f;
    int   window_drag_win_x_   = 0;
    int   window_drag_win_y_   = 0;
public:
    float getVideoZoom() const  { return video_zoom_; }
    float getVideoPanX() const  { return video_pan_x_; }
    float getVideoPanY() const  { return video_pan_y_; }
private:
    void resetVideoZoomPan();

    Playlist playlist_;
    std::vector<std::string> recent_files_;

    // Persistent settings (banana.config next to exe). Load on ctor,
    // save on dtor and after non-trivial state changes (volume snap,
    // subtitle size, always-on-top, repeat/shuffle, av sync).
    void loadConfig();
    void saveConfig();
    bool config_dirty_ = false;
    void markConfigDirty() { config_dirty_ = true; }
    // Recent files list (splash + File > Open Recent) and last-session
    // playlist snapshot (Save Playlist on quit, Restore on start) live in
    // sidecar files, not in banana.config, so a corrupt path in one doesn't
    // wipe the rest of the settings. Format: one absolute path per line.
    void loadRecents();
    void saveRecents();
    void loadLastPlaylist();
    void saveLastPlaylist();
    // Restore the last-known window position + size on startup, and
    // capture the current geometry on shutdown. Called from main.cpp
    // through public thin wrappers. Sidecar: banana.window (key=val).
    void applyStoredWindowGeometry();
    void captureWindowGeometry();

    // Open Folder + M3U playlist file helpers.
    std::string openFolderDialog();
    std::string savePlaylistDialog();
    std::string openPlaylistDialog();
    void openFolderAsPlaylist();
    void savePlaylistM3U(const std::string& path);
    void loadPlaylistM3U(const std::string& path);
    static bool isVideoFile(const std::string& path);
    // Enumerate video files directly under `dir` (non-recursive), returned
    // sorted by file size descending. Dropping a folder onto Banana Player
    // usually means "watch the main video" - the biggest file - not the
    // alphabetically-first thumbnail / sample / behind-the-scenes clip.
    static std::vector<std::string> scanFolderVideosBySize(const std::string& dir);

    // Per-file resume position, VLC-style. Loaded on startup, updated on file
    // switch / natural end / quit, and flushed to disk when dirty.
    struct ResumeEntry { std::string path; double pos = 0.0; double duration = 0.0; };
    std::vector<ResumeEntry> resume_index_;
    bool  resume_dirty_ = false;

    void   loadResumeIndex();
    void   writeResumeIndex();
    double lookupResume(const std::string& path) const;
    void   recordResume(const std::string& path, double pos, double duration);
    void   clearResume(const std::string& path);
    void   snapshotCurrentResume();  // save current playing file's position

    // Ctrl+W: unloads the video, resets window title, keeps playlist so
    // the user can pick another entry. Called from the keyboard handler
    // and the right-click context menu.
    void   closeCurrent();
    // Playlist row context-menu helpers. jumpToPlaylistIndex sets the
    // current index and loads the file; the other two are Windows shell
    // conveniences invoked from the per-row right-click popup.
    void   jumpToPlaylistIndex(size_t i);
    void   revealInExplorer(const std::string& path);
    void   copyPathToClipboard(const std::string& path);
    // Ctrl+Shift+S: copies the current frame to the Windows clipboard as
    // a device-independent bitmap so it can be pasted into any app.
    void   snapshotToClipboard();
    // Ctrl+V: read whatever is on the Windows clipboard - a dragged file
    // (CF_HDROP), a URL, or a plain path (CF_UNICODETEXT) - and try to
    // play it immediately. URLs go straight to FFmpeg's avformat_open_input;
    // directory paths expand into a playlist just like drag-drop of a folder.
    void   pasteAndPlay();
    // Fires the "yt-dlp is N days old" nudge exactly once per session so
    // repeated YouTube pastes don't keep spamming the OSD. Reset only on
    // process restart - intentional; the point is a gentle reminder.
    bool   yt_outdated_warned_ = false;
    // True if the given path's extension is one of our supported subtitle
    // formats (.srt, .smi). Used by the drop handler and the file dialog.
    static bool isSubtitleFile(const std::string& path);

    // Seek-drag state: pause playback while the user drags the seek bar,
    // then resume if it was playing before the drag started. Additional
    // rate-limiting fields prevent the demux thread from thrashing on
    // every mouse pixel: during drag we track the pending target locally
    // and issue player_->seek() at most every ~120 ms wallclock. On
    // release the final target is always committed. Without this, rapid
    // drag would flush + re-decode the pipeline dozens of times per
    // second and paused viewers saw stray intermediate frames catch up.
    bool seek_dragging_ = false;
    bool seek_resume_on_release_ = false;
    float  seek_pending_val_ = 0.0f;
    Uint64 seek_last_issue_ticks_ = 0;
    float  seek_last_issued_val_ = -1.0f;
    // Anchor for shift-slow-scrub. Snapshotted at drag-start so the target
    // is a proportional offset from the drag origin, not the raw slider
    // value. Result: holding Shift while dragging maps mouse motion to
    // ~1/5 of a normal drag, letting the user land on an exact frame.
    float  seek_drag_anchor_val_ = 0.0f;

    // Screen-space rect of the SeekBar last frame, so the wheel handler
    // can seek (instead of adjusting volume) when the cursor is over the
    // seek bar. Stored as (min_x, min_y, max_x, max_y). Zeroed each frame
    // when the bottom overlay is hidden.
    ImVec4 last_seekbar_rect_{0, 0, 0, 0};

    // Right-click context menu state - request open + last cursor pos so
    // BeginPopup positions the menu correctly.
    bool  request_context_menu_ = false;
    ImVec2 context_menu_pos_{0, 0};

    // Defer single-click play/pause until we know a follow-up second click
    // isn't coming (a real double-click = fullscreen). The wait uses the
    // Windows-configured GetDoubleClickTime() BUT capped at 220 ms so users
    // stuck with the default 500 ms setting don't feel a beat of lag on
    // every click. Empirically 200 - 250 ms catches almost every real
    // double-click; browsers use similar caps. Timer counts down in
    // render(); a clicks>=2 event cancels it before it fires.
    bool  pending_video_click_ = false;
    float pending_video_click_timer_ = 0.0f;

    // A-B segment loop. -1 = unset. Press A to advance the state machine:
    // both unset -> set A; A set -> set B (must be > A); both set -> clear.
    double loop_a_ = -1.0;
    double loop_b_ = -1.0;
    void cycleABLoop();
    void checkABLoop();

    // Container chapters (MKV / MP4), cached at loadPath so the render loop
    // doesn't walk fmt_ctx_->chapters every frame. Empty when the file has
    // no chapters or nothing is loaded. Rendered as small ticks on the seek
    // bar; Alt+Left/Right jump between them.
    std::vector<VideoPlayer::Chapter> chapters_;
    void seekPrevChapter();
    void seekNextChapter();

    // User-set bookmarks (Ctrl+M) - per-file, persisted to a `banana.bookmarks`
    // sidecar next to the exe. Same MRU pattern as resume_index_. Rendered
    // as amber diamond ticks below the seek track so they're visually
    // distinct from chapter posts (sky-blue) and A-B loop markers.
    struct BookmarkEntry {
        std::string         path;
        std::vector<double> times;   // seconds, sorted ascending
    };
    std::vector<BookmarkEntry> bookmark_index_;
    std::vector<double>        bookmarks_current_;
    bool                       bookmark_dirty_ = false;
    void loadBookmarkIndex();
    void writeBookmarkIndex();
    void loadBookmarksForCurrent();     // pulls times for the currently-playing file
    void writeBookmarksForCurrent();    // stores current times back to the index
    void addBookmark();                 // Ctrl+M - insert at current playhead
    void removeBookmark(size_t idx);
    void clearBookmarks();

    // Sidecar subtitle tracks discovered next to the video (movie.ko.srt,
    // movie.en.srt, ...). Populated at loadPath. UI presents a picker
    // under Settings > Subtitles when the vector has more than one entry.
    std::vector<SubtitleTrack> subtitle_tracks_;
    int subtitle_track_active_ = -1;   // index into subtitle_tracks_, -1 = none loaded

    // External subtitles (auto-loaded from basename.srt / basename.smi).
    std::vector<SubtitleCue> subtitles_;
    bool subtitles_enabled_ = true;
    int  subtitle_cache_idx_ = 0;     // cache to skip linear scan every frame
    // Secondary subtitle track for dual-subtitle display (learn-a-language
    // workflow: primary in native tongue, secondary in target language).
    // -1 = no secondary loaded. Rendered below the primary line with a
    // slightly smaller font and dimmer color so the eye picks the primary
    // as the main line. All the drawing/timing paths mirror the primary
    // (offset, size, background) so users don't need parallel controls.
    std::vector<SubtitleCue> subtitles_secondary_;
    int  subtitle_secondary_track_active_ = -1;
    int  subtitle_secondary_cache_idx_ = 0;
    const SubtitleCue* currentSubtitleSecondary(double t);
    bool loadSubtitleSidecarSecondary(const std::string& path);
    // Embedded bitmap (PGS / DVB / DVD) subtitle rendering. Keeps one
    // cached SDL_Texture for the currently-visible cue, keyed by the
    // VideoPlayer-assigned cue id, so we don't re-upload the same RGBA
    // bytes every frame. Torn down when the cue id changes (new cue)
    // or vanishes (no cue on screen).
    SDL_Texture* sub_bitmap_tex_ = nullptr;
    uint64_t     sub_bitmap_tex_id_ = 0;
    int          sub_bitmap_tex_w_  = 0;
    int          sub_bitmap_tex_h_  = 0;
    void freeBitmapSubtitleTexture();
    void drawBitmapSubtitle(int W, int H);
    SubtitleSize subtitle_size_ = SubtitleSize::Medium;
    // Positive offset displays subs LATER (add to lookup time); negative
    // displays them EARLIER. Persistent across files (many subs from the
    // same source share the same drift).
    double subtitle_offset_ = 0.0;
    // Vertical position tweak as a fraction of viewport height. Positive
    // values raise the subtitle line above its default resting point (just
    // above the bottom-overlay area); negative values lower it. Alt+Up /
    // Alt+Down step by ~3 %; Alt+0 resets. Persistent so a user who wants
    // subs above burned-in captions doesn't re-nudge every launch.
    float subtitle_vpos_ = 0.0f;
    // Draw a semi-transparent rounded background behind subtitle lines
    // when true. Toggled by plain B; persisted. Helps on busy / high-key
    // backgrounds where the outline alone isn't enough.
    bool subtitle_bg_ = false;
    // Subtitle text color preset. Kept as an enum (not a raw color)
    // so persisted values stay meaningful across UI restyles and users
    // can't pick unreadable low-contrast garbage. White is the default;
    // Yellow matches the classic Korean-TV subtitle look (a common ask
    // from GomPlayer/PotPlayer users); Cyan/SkyBlue/Pink cover the rest.
    enum class SubColor { White, Yellow, Cyan, SkyBlue, Pink };
    SubColor subtitle_color_ = SubColor::White;
    // Outline (letterbox around each character) thickness. Thin is the
    // current 1-px 8-neighbor pass; Normal doubles to 2 px so subs stay
    // legible on bright/busy backgrounds; Thick is 3 px for HDR content
    // or projectors where even Normal washes out.
    enum class SubOutline { Thin, Normal, Thick };
    SubOutline subtitle_outline_ = SubOutline::Thin;
    // Subtitle font preset. Values map to Style::SubtitleFont enum -
    // kept as a plain int here to avoid a compile-time dependency on
    // Style.hpp in a header. drawSubtitles casts on use.
    int subtitle_font_ = 0;    // 0 = default merged, 1 = MalgunBold, 2 = Gulim, 3 = Batang
    void loadCompanionSubs(const std::string& video_path);
    const SubtitleCue* currentSubtitle(double t);
    void drawSubtitles(int W, int H);
    void cycleSubtitleSize();
    // Single toggle path for HDR tone mapping - shared by the keyboard
    // shortcut (Shift+H), the Settings menu item, and the overlay pill.
    // Handles state flip, player sync, queue flush (so the switch shows
    // immediately), OSD, and config dirty marking.
    void toggleHdrToneMap();
    // Cycle to the next available audio track. Wraps around the last
    // track back to the first. No-op when only one (or zero) tracks are
    // present.
    void cycleAudioTrack();
    // Cycle to the next external subtitle sidecar (movie.ko.srt ->
    // movie.en.srt -> ... -> back to first). No-op when 0 or 1 sidecar
    // tracks were discovered. Bound to J (mpv convention).
    void cycleSubtitleTrack();
    float subtitleBasePx(int W, int H) const;
    // Shared subtitle-load path used by both the drag-drop handler and
    // the Settings > Subtitles > Load Subtitle... menu item. Returns true
    // on parse success; caller decides whether to OSD or not. Loads the
    // file as UTF-8 (or CP949 fallback), parses SRT / SMI by extension,
    // and swaps in the resulting cues without touching playback state.
    bool loadSubtitleSidecar(const std::string& path);
    // Presents a Win32 Open File dialog filtered to .srt / .smi and,
    // on success, calls loadSubtitleSidecar with an appropriate OSD.
    // No-ops with an OSD if no video is currently loaded.
    void openSubtitleDialog();

    // Ctrl+= : resize the window so its client area matches the video's
    // post-rotation aspect ratio, filling up to ~85 % of the current
    // display's usable bounds. No-ops (with OSD) if no video is loaded
    // or the window is fullscreen; un-maximizes first if maximized.
    void snapWindowToVideo();

    // Resize the window so its client area matches (scale * native video
    // pixels), post-rotation. scale = 0.5 / 1.0 / 2.0 are the 1/2/3 hotkeys.
    // Clamps to the display's usable bounds and the 640x400 minimum. Also
    // resets video_zoom_ / pan so the video fills the new client area
    // edge-to-edge rather than being letterboxed inside a scaled view.
    void snapWindowToVideoScale(float scale);

    // Seek-bar hover thumbnail preview. Off by default; togglable from the
    // Settings menu and persisted to banana.config. When enabled AND the
    // cursor is over the seek bar AND we're not mid-drag, we ask the
    // ThumbnailProvider for a preview and float a small popup above the
    // cursor. The provider owns its own AVFormatContext + worker thread so
    // it never contends with the primary decode pipeline.
    std::unique_ptr<ThumbnailProvider> thumb_provider_;
    // Default on. This is the single most-requested seekbar feature
    // across the VLC/mpv/PotPlayer forums (thumbfast, mpv_thumbnail_script,
    // the perennial "why doesn't VLC do this" threads). The provider
    // runs on its own thread + AVFormatContext so first-hover is a cheap
    // decode miss, not a decode-pipeline hitch.
    bool thumbnail_preview_enabled_ = true;
    void drawSeekBarThumbnail(int W, int H, float duration);

    // Background yt-dlp resolver for pasted YouTube URLs. Owns its own
    // worker thread; the render loop drains completed resolutions and
    // hands the resulting direct media URL to loadPath. pending_yt_*
    // tracks the most recent request so stale results (from a superseded
    // cancellation) are ignored.
    std::unique_ptr<YoutubeResolver> yt_resolver_;
    uint64_t     pending_yt_request_id_ = 0;
    std::string  pending_yt_source_url_;

    // Source YouTube page URL for the currently-playing video (empty when
    // playback isn't from a YouTube resolve). Distinct from pending_yt_*:
    // the pending one clears the moment the resolve completes; this one
    // stays set as long as the YouTube-resolved stream plays, so quality
    // changes mid-playback can re-resolve the same source URL.
    std::string  current_yt_source_url_;
    YoutubeQuality youtube_quality_ = YoutubeQuality::Auto;

    // Internal load path used by the render drain block after a YouTube
    // resolve completes. Same as loadPath() except it does NOT clear
    // current_yt_source_url_ - we still need that URL to re-resolve if
    // the user changes quality later.
    bool loadResolvedYoutubeUrl(const std::string& media_url);

    // Modal help panels.
    bool show_shortcuts_ = false;
    bool show_about_    = false;
    bool show_info_     = false;
    // Quick file-info overlay toggled by Tab (PotPlayer convention).
    // Compact card in the top-left with the essentials: filename,
    // codecs, resolution, fps, bitrate, keyframe interval, path.
    // Tab again or Esc closes. Independent of `show_info_` (the
    // detailed panel opened by I / F1).
    bool show_quick_info_ = false;
    // Playback history modal (Ctrl+H): scrollable list of previously-watched
    // files from resume_index_ and recent_files_, with progress bars and
    // click-to-continue. Deferred mutations avoid modifying resume_index_
    // during the ImGui loop.
    bool show_history_  = false;
    // Settings dialog (Ctrl+,): tabbed configuration surface. Owns the
    // long-tail preference items (audio latency, HDR half-res, deint,
    // normalizer, subtitle default styling, snapshot behaviour, etc.)
    // that used to clutter the main menu. Per-file / per-session
    // controls (rotation, playlist, audio track, subtitle track pick)
    // stay in the main menu since they're touched often.
    bool show_settings_ = false;
    // Banana Player Pro license entry buffers (About panel). Persisted
    // via License::activate() on Verify click; the buffers themselves
    // are session-only. Kept as fixed-size arrays so ImGui::InputText
    // can write directly without an alloc.
    char        license_email_buf_[128] = {0};
    char        license_key_buf_[128]    = {0};
    std::string license_msg_;   // shown next to Verify: green/red status
    int  settings_tab_  = 0;   // active tab index across draws
    // Goto-time dialog (Ctrl+G): tiny modal that takes "H:MM:SS",
    // "MM:SS", or plain seconds and seeks there. Standard mpv / VLC
    // convention for jumping to a known timestamp precisely.
    bool show_goto_time_ = false;
    bool goto_focus_pending_ = false;
    char goto_time_buf_[32] = {0};
    void drawShortcutsPanel(int W, int H);
    void drawAboutPanel(int W, int H);
    void drawInfoPanel(int W, int H);
    void drawQuickInfoOverlay(int W, int H);
    void drawHistoryPanel(int W, int H);
    void drawSettingsDialog(int W, int H);
    void drawGotoTimeDialog(int W, int H);
};
