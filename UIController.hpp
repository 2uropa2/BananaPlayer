#pragma once

#include <SDL3/SDL.h>
#include "imgui.h"
#include "VideoPlayer.hpp"
#include "Playlist.hpp"
#include "Style.hpp"
#include "Subtitles.hpp"

#include <functional>
#include <string>
#include <vector>

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

class UIController {
public:
    UIController(VideoPlayer* player, SDL_Window* window, SDL_Renderer* renderer);
    ~UIController();

    void newFrame();
    void render(int windowW, int windowH);
    void handleEvent(const SDL_Event& event);

    bool isFullscreen() const { return fullscreen_; }
    void setFullscreen(bool val);

    void openFileDialog();
    // Returns true if the file loaded and playback started; false on failure
    // (a "Cannot open..." OSD is shown either way). Callers that walk the
    // playlist use the return value to skip broken items.
    bool loadPath(const std::string& path);
    void enqueuePaths(const std::vector<std::string>& paths, bool play_first = true);

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

    // External subtitles (auto-loaded from basename.srt / basename.smi).
    std::vector<SubtitleCue> subtitles_;
    bool subtitles_enabled_ = true;
    int  subtitle_cache_idx_ = 0;     // cache to skip linear scan every frame
    SubtitleSize subtitle_size_ = SubtitleSize::Medium;
    // Positive offset displays subs LATER (add to lookup time); negative
    // displays them EARLIER. Persistent across files (many subs from the
    // same source share the same drift).
    double subtitle_offset_ = 0.0;
    void loadCompanionSubs(const std::string& video_path);
    const SubtitleCue* currentSubtitle(double t);
    void drawSubtitles(int W, int H);
    void cycleSubtitleSize();
    float subtitleBasePx(int W) const;
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

    // Modal help panels.
    bool show_shortcuts_ = false;
    bool show_about_    = false;
    bool show_info_     = false;
    void drawShortcutsPanel(int W, int H);
    void drawAboutPanel(int W, int H);
    void drawInfoPanel(int W, int H);
};
