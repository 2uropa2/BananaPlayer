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

    std::function<void()> onExitRequested;

private:
    void drawSplash(int W, int H);
    void drawTopOverlay(int W, int H, float alpha);
    void drawBottomOverlay(int W, int H, float alpha);
    void drawPlaylistPanel(int W, int H, float alpha);
    void drawOsd(int W, int H);
    void drawFlash(int W, int H);
    void drawMainMenuContents();

    void showOsd(const std::string& text, float duration = 1.6f);
    void takeSnapshot();
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

    Playlist playlist_;
    std::vector<std::string> recent_files_;

    // Persistent settings (vela.config next to exe). Load on ctor,
    // save on dtor and after non-trivial state changes (volume snap,
    // subtitle size, always-on-top, repeat/shuffle, av sync).
    void loadConfig();
    void saveConfig();
    bool config_dirty_ = false;
    void markConfigDirty() { config_dirty_ = true; }
    // Recent files list (splash + File > Open Recent) and last-session
    // playlist snapshot (Save Playlist on quit, Restore on start) live in
    // sidecar files, not in vela.config, so a corrupt path in one doesn't
    // wipe the rest of the settings. Format: one absolute path per line.
    void loadRecents();
    void saveRecents();
    void loadLastPlaylist();
    void saveLastPlaylist();
    // Restore the last-known window position + size on startup, and
    // capture the current geometry on shutdown. Called from main.cpp
    // through public thin wrappers. Sidecar: vela.window (key=val).
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

    // Screen-space rect of the SeekBar last frame, so the wheel handler
    // can seek (instead of adjusting volume) when the cursor is over the
    // seek bar. Stored as (min_x, min_y, max_x, max_y). Zeroed each frame
    // when the bottom overlay is hidden.
    ImVec4 last_seekbar_rect_{0, 0, 0, 0};

    // Right-click context menu state - request open + last cursor pos so
    // BeginPopup positions the menu correctly.
    bool  request_context_menu_ = false;
    ImVec2 context_menu_pos_{0, 0};

    // Toggle-and-undo pattern for click play/pause: the first left-click
    // toggles immediately so it feels responsive. If a second click arrives
    // (double-click-to-fullscreen), we undo the toggle so the net effect is
    // just fullscreen. This flag records that the first click's toggle is
    // still standing and can be reverted.
    bool  click_toggle_undoable_ = false;

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

    // User-set bookmarks (Ctrl+M) - per-file, persisted to a `vela.bookmarks`
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
    void loadCompanionSubs(const std::string& video_path);
    const SubtitleCue* currentSubtitle(double t);
    void drawSubtitles(int W, int H);
    void cycleSubtitleSize();
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

    // Seek-bar hover thumbnail preview. Off by default; togglable from the
    // Settings menu and persisted to vela.config. When enabled AND the
    // cursor is over the seek bar AND we're not mid-drag, we ask the
    // ThumbnailProvider for a preview and float a small popup above the
    // cursor. The provider owns its own AVFormatContext + worker thread so
    // it never contends with the primary decode pipeline.
    std::unique_ptr<ThumbnailProvider> thumb_provider_;
    bool thumbnail_preview_enabled_ = false;
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
    // Playback history modal (Ctrl+H): scrollable list of previously-watched
    // files from resume_index_ and recent_files_, with progress bars and
    // click-to-continue. Deferred mutations avoid modifying resume_index_
    // during the ImGui loop.
    bool show_history_  = false;
    void drawShortcutsPanel(int W, int H);
    void drawAboutPanel(int W, int H);
    void drawInfoPanel(int W, int H);
    void drawHistoryPanel(int W, int H);
};
