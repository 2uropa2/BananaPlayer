#include "UIController.hpp"
#include "Thumbnail.hpp"
#include "Version.hpp"
#include "Youtube.hpp"

#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

// Seek nudges (+/-5s, +/-30s, frame step) fire while the user is watching and
// the seek bar already reflects the change - the OSD popup on top of it feels
// noisy. Keep them in Debug for troubleshooting, drop them in Release.
#ifdef NDEBUG
    #define SEEK_OSD(msg) ((void)0)
#else
    #define SEEK_OSD(msg) showOsd(msg)
#endif

// -------------- Local helpers --------------

namespace {

const char* baseName(const char* s) {
    const char* p1 = std::strrchr(s, '\\');
    const char* p2 = std::strrchr(s, '/');
    const char* last = (p1 && p2) ? (p1 > p2 ? p1 : p2) : (p1 ? p1 : p2);
    return last ? last + 1 : s;
}

std::string formatTime(double seconds) {
    if (!(seconds >= 0.0) || seconds > 359999.0) seconds = 0.0;
    int total = (int)seconds;
    int h = total / 3600;
    int m = (total % 3600) / 60;
    int s = total % 60;
    char buf[32];
    if (h > 0) std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
    else       std::snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return buf;
}

std::string appDir() {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return ".";
    std::string s(buf, n);
    size_t p = s.find_last_of("\\/");
    return (p == std::string::npos) ? "." : s.substr(0, p);
}

std::string timestampString() {
    std::time_t t = std::time(nullptr);
    std::tm lt{};
    localtime_s(&lt, &t);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d",
                  lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                  lt.tm_hour, lt.tm_min, lt.tm_sec);
    return buf;
}

std::string resumeIndexPath() {
    // Sits next to the exe, same convention as snapshots.
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return "vela.resume";
    std::string s(buf, n);
    size_t p = s.find_last_of("\\/");
    return (p == std::string::npos) ? "vela.resume" : (s.substr(0, p) + "\\vela.resume");
}

std::string bookmarkIndexPath() {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return "vela.bookmarks";
    std::string s(buf, n);
    size_t p = s.find_last_of("\\/");
    return (p == std::string::npos) ? "vela.bookmarks"
                                    : (s.substr(0, p) + "\\vela.bookmarks");
}

std::string openFileDialogWin() {
    OPENFILENAMEA ofn = {};
    char szFile[MAX_PATH] = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter =
        "Media Files\0"
        "*.mp4;*.mkv;*.avi;*.mov;*.webm;*.wmv;*.flv;*.m4v;*.mpg;*.mpeg;*.ts;*.3gp;"
        "*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp;*.tif;*.tiff\0"
        "Video Files\0*.mp4;*.mkv;*.avi;*.mov;*.webm;*.wmv;*.flv;*.m4v;*.mpg;*.mpeg;*.ts;*.3gp\0"
        "Image Files\0*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp;*.tif;*.tiff\0"
        "All Files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (GetOpenFileNameA(&ofn)) return std::string(szFile);
    return {};
}

} // namespace

// -------------- UIController --------------

UIController::UIController(VideoPlayer* player, SDL_Window* window, SDL_Renderer* renderer)
    : player_(player), window_(window), renderer_(renderer) {
    player_->onEnded = [this]() { handleNaturalEnd(); };
    thumb_provider_ = std::make_unique<ThumbnailProvider>(renderer_);
    yt_resolver_    = std::make_unique<YoutubeResolver>();
    loadResumeIndex();
    loadBookmarkIndex();
    loadConfig();
    loadRecents();
    loadLastPlaylist();
    // NOTE: window geometry restore intentionally disabled - the earlier
    // implementation left users with a window pinned to (0, 0) that
    // couldn't be dragged (SDL_MaximizeWindow + a bad clamp interaction
    // on the CENTERED sentinel). Re-enable only after both are fixed.
    // applyStoredWindowGeometry();
}

UIController::~UIController() {
    snapshotCurrentResume();
    if (resume_dirty_) writeResumeIndex();
    if (bookmark_dirty_) writeBookmarkIndex();
    saveConfig();
    saveRecents();
    saveLastPlaylist();
    // captureWindowGeometry();   // See ctor note.
    // Defensive: if main.cpp forgot to call shutdownBeforeSdl(), destroy
    // the thumbnail provider here anyway. On this path SDL_DestroyRenderer
    // has already run and the SDL_DestroyTexture calls become no-ops on
    // stale handles - not a crash, just leaked GPU memory the OS reclaims
    // on process exit.
    thumb_provider_.reset();
}

void UIController::shutdownBeforeSdl() {
    // Tear down the thumbnail provider (worker thread + textures) while
    // the SDL renderer is still valid, so cache-eviction SDL_DestroyTexture
    // calls hit a live renderer. Called from main.cpp just before
    // ImGui/SDL shutdown.
    thumb_provider_.reset();
}

void UIController::loadResumeIndex() {
    resume_index_.clear();
    std::FILE* f = std::fopen(resumeIndexPath().c_str(), "rb");
    if (!f) return;
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), f)) {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line.empty()) continue;
        size_t t1 = line.find('\t');
        size_t t2 = (t1 == std::string::npos) ? std::string::npos : line.find('\t', t1 + 1);
        if (t1 == std::string::npos || t2 == std::string::npos) continue;
        ResumeEntry e;
        e.pos      = std::atof(line.substr(0, t1).c_str());
        e.duration = std::atof(line.substr(t1 + 1, t2 - t1 - 1).c_str());
        e.path     = line.substr(t2 + 1);
        if (!e.path.empty()) resume_index_.push_back(std::move(e));
    }
    std::fclose(f);
    if (resume_index_.size() > 200) resume_index_.resize(200);
}

void UIController::writeResumeIndex() {
    std::FILE* f = std::fopen(resumeIndexPath().c_str(), "wb");
    if (!f) return;
    size_t n = std::min<size_t>(resume_index_.size(), 200);
    for (size_t i = 0; i < n; ++i) {
        const auto& e = resume_index_[i];
        std::fprintf(f, "%.3f\t%.3f\t%s\n", e.pos, e.duration, e.path.c_str());
    }
    std::fclose(f);
    resume_dirty_ = false;
}

double UIController::lookupResume(const std::string& path) const {
    for (const auto& e : resume_index_) {
        if (e.path == path) return e.pos;
    }
    return -1.0;
}

void UIController::recordResume(const std::string& path, double pos, double duration) {
    if (path.empty()) return;
    // Erase any prior entry and re-insert at front (MRU).
    resume_index_.erase(
        std::remove_if(resume_index_.begin(), resume_index_.end(),
                       [&](const ResumeEntry& e){ return e.path == path; }),
        resume_index_.end());
    ResumeEntry e{path, pos, duration};
    resume_index_.insert(resume_index_.begin(), std::move(e));
    if (resume_index_.size() > 200) resume_index_.resize(200);
    resume_dirty_ = true;
}

void UIController::clearResume(const std::string& path) {
    auto n_before = resume_index_.size();
    resume_index_.erase(
        std::remove_if(resume_index_.begin(), resume_index_.end(),
                       [&](const ResumeEntry& e){ return e.path == path; }),
        resume_index_.end());
    if (resume_index_.size() != n_before) resume_dirty_ = true;
}

// ---------- Bookmarks (Ctrl+M) ----------

void UIController::loadBookmarkIndex() {
    bookmark_index_.clear();
    std::FILE* f = std::fopen(bookmarkIndexPath().c_str(), "rb");
    if (!f) return;
    // Line format:  t1,t2,t3\t<path>
    // Times are decimal seconds ascending.
    char buf[8192];
    while (std::fgets(buf, sizeof(buf), f)) {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line.empty()) continue;
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        BookmarkEntry e;
        e.path = line.substr(tab + 1);
        if (e.path.empty()) continue;
        std::string times = line.substr(0, tab);
        size_t pos = 0;
        while (pos < times.size()) {
            size_t comma = times.find(',', pos);
            if (comma == std::string::npos) comma = times.size();
            double t = std::atof(times.substr(pos, comma - pos).c_str());
            if (t >= 0.0) e.times.push_back(t);
            pos = comma + 1;
        }
        std::sort(e.times.begin(), e.times.end());
        if (!e.times.empty()) bookmark_index_.push_back(std::move(e));
    }
    std::fclose(f);
    if (bookmark_index_.size() > 200) bookmark_index_.resize(200);
}

void UIController::writeBookmarkIndex() {
    std::FILE* f = std::fopen(bookmarkIndexPath().c_str(), "wb");
    if (!f) return;
    size_t n = std::min<size_t>(bookmark_index_.size(), 200);
    for (size_t i = 0; i < n; ++i) {
        const auto& e = bookmark_index_[i];
        if (e.times.empty()) continue;
        for (size_t j = 0; j < e.times.size(); ++j) {
            std::fprintf(f, "%s%.3f", j ? "," : "", e.times[j]);
        }
        std::fprintf(f, "\t%s\n", e.path.c_str());
    }
    std::fclose(f);
    bookmark_dirty_ = false;
}

void UIController::loadBookmarksForCurrent() {
    bookmarks_current_.clear();
    if (!player_->isLoaded()) return;
    const std::string& p = player_->getFilename();
    if (p.empty()) return;
    for (const auto& e : bookmark_index_) {
        if (e.path == p) { bookmarks_current_ = e.times; return; }
    }
}

void UIController::writeBookmarksForCurrent() {
    if (!player_->isLoaded()) return;
    const std::string& p = player_->getFilename();
    if (p.empty()) return;
    // Drop any existing entry for this path.
    bookmark_index_.erase(
        std::remove_if(bookmark_index_.begin(), bookmark_index_.end(),
                       [&](const BookmarkEntry& e){ return e.path == p; }),
        bookmark_index_.end());
    if (!bookmarks_current_.empty()) {
        BookmarkEntry e;
        e.path  = p;
        e.times = bookmarks_current_;
        std::sort(e.times.begin(), e.times.end());
        // MRU: newest file's entry goes to the front.
        bookmark_index_.insert(bookmark_index_.begin(), std::move(e));
        if (bookmark_index_.size() > 200) bookmark_index_.resize(200);
    }
    bookmark_dirty_ = true;
}

void UIController::addBookmark() {
    if (!player_->isLoaded()) { showOsd("No file loaded", 1.4f); return; }
    if (bookmarks_current_.size() >= 50) {
        showOsd("Bookmark limit reached (50)", 2.0f);
        return;
    }
    double t = player_->getCurrentTime();
    // Ignore duplicates within +/- 1 s of an existing bookmark - the user's
    // clicking Ctrl+M twice in a row usually means "one bookmark here".
    for (double existing : bookmarks_current_) {
        if (std::fabs(existing - t) < 1.0) {
            showOsd("Already bookmarked here", 1.6f);
            return;
        }
    }
    bookmarks_current_.push_back(t);
    std::sort(bookmarks_current_.begin(), bookmarks_current_.end());
    writeBookmarksForCurrent();
    char buf[96];
    std::snprintf(buf, sizeof(buf), "Bookmark added at %s",
                  formatTime(t).c_str());
    showOsd(buf, 1.8f);
}

void UIController::removeBookmark(size_t idx) {
    if (idx >= bookmarks_current_.size()) return;
    bookmarks_current_.erase(bookmarks_current_.begin() + idx);
    writeBookmarksForCurrent();
    showOsd("Bookmark removed", 1.4f);
}

void UIController::clearBookmarks() {
    if (bookmarks_current_.empty()) return;
    bookmarks_current_.clear();
    writeBookmarksForCurrent();
    showOsd("Bookmarks cleared", 1.4f);
}

void UIController::snapshotCurrentResume() {
    if (!player_->isLoaded()) return;
    const std::string& p = player_->getFilename();
    if (p.empty()) return;
    double pos = player_->getCurrentTime();
    double dur = player_->getDuration();
    // Only remember positions that are meaningful: past the opening seconds
    // and not right at the end (where the user would want to start over).
    if (pos > 5.0 && dur > 10.0 && pos < dur - 10.0) {
        recordResume(p, pos, dur);
    } else {
        clearResume(p);
    }
}

void UIController::setFullscreen(bool val) {
    fullscreen_ = val;
    SDL_SetWindowFullscreen(window_, val);
    if (val) {
        SDL_HideCursor();
        cursor_visible_ = false;
        ui_visible_timer_ = 2.0f;
        cursor_idle_timer_ = 1.0f;
    } else {
        // Exiting fullscreen: cursor must come back immediately regardless
        // of the previous hidden/shown state. nudgeVisible() below would do
        // it via the !cursor_visible_ branch, but we set the flag here
        // explicitly so the state stays coherent even if callers ever bypass
        // nudgeVisible.
        SDL_ShowCursor();
        cursor_visible_ = true;
    }
    nudgeVisible();
}

void UIController::openFileDialog() {
    std::string path = openFileDialogWin();
    if (!path.empty()) loadPath(path);
}

bool UIController::loadPath(const std::string& path) {
    // Save the OUTGOING file's position before we swap, and flush to disk so
    // a later crash doesn't lose the resume mark for the file we just closed.
    snapshotCurrentResume();
    if (resume_dirty_) writeResumeIndex();
    if (bookmark_dirty_) writeBookmarkIndex();
    bookmarks_current_.clear();

    // Loading a non-YouTube file (direct file/URL): the current YouTube
    // source URL, if any, is now stale. The YouTube-resolve callers use
    // loadResolvedYoutubeUrl() below which saves/restores this so a
    // quality re-resolve doesn't lose the source URL.
    current_yt_source_url_.clear();

    // A-B loop is per-file: clear it when the file changes.
    loop_a_ = -1.0;
    loop_b_ = -1.0;
    // Rotation is a per-file transient - drop the user's Ctrl+R from
    // the previous file so a rotation set for a portrait clip doesn't
    // get carried into the next (landscape) clip and turn it sideways.
    // Intrinsic rotation still applies (it's read fresh from the new
    // file's display-matrix on every render).
    rotation_ = RotationMode::Deg0;
    // Same rationale for A/V sync: yesterday's -300 ms nudge for a lip-
    // sync mismatch on some other file must not silently follow the user
    // into every subsequent load.
    player_->setAVSyncOffset(0.0);
    // Attempt to auto-load companion .srt / .smi.
    loadCompanionSubs(path);

    if (!player_->loadFile(path)) {
        // Load failed - keep the title/state clean and let the user know
        // which file failed via a clear OSD (longer duration than normal).
        char buf[512]; std::snprintf(buf, sizeof(buf),
                                     "Cannot open  %s", baseName(path.c_str()));
        showOsd(buf, 3.0f);
        SDL_SetWindowTitle(window_, "Vela");
        return false;
    }
    {
        // Refresh onEnded (loadFile does not clear it, but be safe)
        if (!player_->onEnded) player_->onEnded = [this]() { handleNaturalEnd(); };
        // Cache container chapters up front; empty vector for files without.
        chapters_ = player_->getChapters();
        // Pull any prior bookmarks for this file into bookmarks_current_.
        loadBookmarksForCurrent();
        addRecent(path);
        // Update playlist current selection to match (or append)
        bool found = false;
        for (size_t i = 0; i < playlist_.size(); ++i) {
            if (playlist_.items()[i] == path) { playlist_.setCurrent(i); found = true; break; }
        }
        if (!found) {
            playlist_.add(path);
            playlist_.setCurrent(playlist_.size() - 1);
        }

        // Restore resume position if we have one AND it's still meaningful for
        // this file's actual duration (guards against replaced files).
        double resume_pos = lookupResume(path);
        double dur = player_->getDuration();
        if (resume_pos > 5.0 && dur > 10.0 && resume_pos < dur - 10.0) {
            player_->seek(resume_pos);
            char buf[64]; std::snprintf(buf, sizeof(buf), "Resumed at %s", formatTime(resume_pos).c_str());
            showOsd(buf, 2.2f);
        }

        player_->play();

        char title[512];
        std::snprintf(title, sizeof(title), "%s  -  Vela",
                      baseName(path.c_str()));
        SDL_SetWindowTitle(window_, title);

        // Announce whether hardware decode is engaged so the user knows.
        if (player_->isHardwareDecoding()) {
            char buf[64]; std::snprintf(buf, sizeof(buf), "HW decode  %s",
                                        player_->getHardwareDecoderName());
            showOsd(buf, 1.4f);
        }

        // Prime the thumbnail provider on the newly-loaded file so hover
        // previews are ready without a first-hover cold miss. openFile is
        // cheap (posts to the worker thread and returns).
        if (thumb_provider_) thumb_provider_->openFile(path);
    }
    return true;
}

void UIController::enqueuePaths(const std::vector<std::string>& paths, bool play_first) {
    if (paths.empty()) return;
    // Expand any directory arguments to their video children so both CLI
    // (`Vela.exe C:\videos`) and drag-drop of a folder do the intuitive
    // thing. Non-directory paths are passed through untouched (FFmpeg can
    // still surface a "Cannot open" for anything it doesn't like).
    std::vector<std::string> expanded;
    expanded.reserve(paths.size());
    for (const auto& p : paths) {
        DWORD attrs = GetFileAttributesA(p.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            WIN32_FIND_DATAA fd{};
            std::string pattern = p + "\\*";
            HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
            std::vector<std::string> found;
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    std::string full = p + "\\" + fd.cFileName;
                    if (isVideoFile(full)) found.push_back(full);
                } while (FindNextFileA(h, &fd));
                FindClose(h);
            }
            std::sort(found.begin(), found.end());
            expanded.insert(expanded.end(), found.begin(), found.end());
        } else {
            expanded.push_back(p);
        }
    }
    if (expanded.empty()) return;
    bool was_empty = playlist_.empty();
    playlist_.addBulk(expanded);
    if (play_first || was_empty) {
        // start playing the first new one
        size_t idx = playlist_.size() - expanded.size();
        playlist_.setCurrent(idx);
        loadPath(playlist_.current());
    }
}

bool UIController::isYoutubeUrl(const std::string& s) {
    // Case-insensitive lowercase copy for prefix / substring checks.
    // Path-and-query parts of a URL are technically case-sensitive on the
    // server side but the host is not, and we only match on host + a small
    // set of well-known path prefixes.
    std::string lower;
    lower.reserve(s.size());
    for (char c : s) lower.push_back((char)std::tolower((unsigned char)c));

    // Strip optional scheme so the host check is uniform.
    const char* p = lower.c_str();
    if (std::strncmp(p, "https://", 8) == 0)      p += 8;
    else if (std::strncmp(p, "http://", 7) == 0)  p += 7;
    // Strip optional www./m./music. prefix on the host.
    if      (std::strncmp(p, "www.", 4) == 0)     p += 4;
    else if (std::strncmp(p, "m.", 2) == 0)       p += 2;
    else if (std::strncmp(p, "music.", 6) == 0)   p += 6;

    // Now p should start with either "youtube.com/..." or "youtu.be/...".
    if (std::strncmp(p, "youtu.be/", 9) == 0) return true;
    if (std::strncmp(p, "youtube.com/", 12) == 0) {
        const char* path = p + 12;
        if (std::strncmp(path, "watch",  5) == 0) return true;
        if (std::strncmp(path, "shorts/", 7) == 0) return true;
        if (std::strncmp(path, "live/",  5) == 0) return true;
        if (std::strncmp(path, "embed/", 6) == 0) return true;
        if (std::strncmp(path, "v/",     2) == 0) return true;
    }
    return false;
}

namespace {

// Human-readable label for the YouTube quality pill / menu items. Kept
// short so the top-overlay pill stays compact at 640x400.
const char* youtubeQualityLabel(YoutubeQuality q) {
    switch (q) {
        case YoutubeQuality::Auto:      return "Auto";
        case YoutubeQuality::Q1080:     return "1080p";
        case YoutubeQuality::Q720:      return "720p";
        case YoutubeQuality::Q480:      return "480p";
        case YoutubeQuality::AudioOnly: return "Audio";
    }
    return "Auto";
}

// yt-dlp -f selector string. Single-file selectors only - the
// video+audio-merged path requires a two-URL refactor of VideoPlayer.
const char* youtubeQualityFormat(YoutubeQuality q) {
    switch (q) {
        case YoutubeQuality::Auto:      return "best";
        case YoutubeQuality::Q1080:     return "best[height<=1080]";
        case YoutubeQuality::Q720:      return "best[height<=720]";
        case YoutubeQuality::Q480:      return "best[height<=480]";
        case YoutubeQuality::AudioOnly: return "bestaudio";
    }
    return "best";
}

// Parse the persisted string form (auto|1080|720|480|audio) back to the
// enum. Unknown values fall back to Auto so hand-edited configs don't
// break future starts.
YoutubeQuality youtubeQualityFromString(const char* s) {
    if      (std::strcmp(s, "auto")  == 0) return YoutubeQuality::Auto;
    else if (std::strcmp(s, "1080")  == 0) return YoutubeQuality::Q1080;
    else if (std::strcmp(s, "720")   == 0) return YoutubeQuality::Q720;
    else if (std::strcmp(s, "480")   == 0) return YoutubeQuality::Q480;
    else if (std::strcmp(s, "audio") == 0) return YoutubeQuality::AudioOnly;
    return YoutubeQuality::Auto;
}

const char* youtubeQualityToString(YoutubeQuality q) {
    switch (q) {
        case YoutubeQuality::Auto:      return "auto";
        case YoutubeQuality::Q1080:     return "1080";
        case YoutubeQuality::Q720:      return "720";
        case YoutubeQuality::Q480:      return "480";
        case YoutubeQuality::AudioOnly: return "audio";
    }
    return "auto";
}

} // namespace

void UIController::openAny(const std::string& path) {
    if (isYoutubeUrl(path)) {
        // Kick off a background yt-dlp resolve. The user sees the OSD
        // immediately so they know the click landed; when the direct URL
        // arrives (typically 2-5 s later) the render loop drains the
        // resolver and hands it off to loadPath.
        if (!yt_resolver_) {
            showOsd("YouTube resolver unavailable", 2.5f);
            return;
        }
        YtDlpProbeState pst = yt_resolver_->probeState();
        if (pst == YtDlpProbeState::Missing) {
            // Front-run the drain so first-time users get actionable
            // guidance without a delay. The resolver will also enqueue
            // the same failure result; we just show the OSD once here.
            showOsd("yt-dlp not found - install: winget install yt-dlp", 4.5f);
        } else {
            // Probing OR Available - either way the worker takes it. If
            // still probing, the request sits in has_pending_ until phase
            // 1 finishes (~500 ms after startup at worst), then proceeds
            // normally. Age nudge only fires once probe is done (age is
            // -1 in the probing state so the > 30 test naturally skips).
            int age = yt_resolver_->ytDlpAgeDays();
            if (age > 30 && !yt_outdated_warned_) {
                char buf[128];
                std::snprintf(buf, sizeof(buf),
                              "yt-dlp is %d days old - winget upgrade yt-dlp", age);
                showOsd(buf, 4.5f);
                yt_outdated_warned_ = true;
            }
            showOsd("Resolving YouTube...", 10.0f);
        }
        // Remember the source page URL BEFORE dispatching so the top-
        // overlay quality pill has something to show as soon as playback
        // begins. Also so that a quality change mid-playback can re-
        // resolve the same source URL.
        current_yt_source_url_ = path;
        pending_yt_request_id_ = yt_resolver_->requestResolve(
            path, youtubeQualityFormat(youtube_quality_));
        pending_yt_source_url_ = path;
        return;
    }
    loadPath(path);
}

bool UIController::loadResolvedYoutubeUrl(const std::string& media_url) {
    // Preserve the source URL across the loadPath() call - loadPath clears
    // current_yt_source_url_ unconditionally so a subsequent direct-file
    // load doesn't hold onto a stale YouTube badge.
    std::string saved = current_yt_source_url_;
    bool ok = loadPath(media_url);
    current_yt_source_url_ = std::move(saved);
    return ok;
}

void UIController::addRecent(const std::string& path) {
    auto it = std::find(recent_files_.begin(), recent_files_.end(), path);
    if (it != recent_files_.end()) recent_files_.erase(it);
    recent_files_.insert(recent_files_.begin(), path);
    if (recent_files_.size() > 12) recent_files_.resize(12);
}

void UIController::showOsd(const std::string& text, float duration) {
    OsdMsg m{ text, duration, duration };
    osd_.push_back(std::move(m));
}

void UIController::nudgeVisible() {
    ui_visible_timer_ = 3.0f;
    overlay_target_ = 1.0f;
    // Cursor gets its own (shorter) idle timer. Reset alongside the overlay
    // timer on every real activity nudge, and force the cursor back on if
    // we had hidden it - so any mouse motion / key press / wheel event
    // brings the pointer back instantly.
    cursor_idle_timer_ = 1.0f;
    if (!cursor_visible_) {
        SDL_ShowCursor();
        cursor_visible_ = true;
    }
}

void UIController::takeSnapshot() {
    if (!player_->isLoaded()) return;
    std::string dir  = appDir();
    std::string name = std::string("snapshot_") + timestampString() + ".png";
    std::string full = dir + "\\" + name;
    if (player_->snapshot(full)) {
        snapshot_flash_ = 1.0f;
        showOsd(std::string("Saved  ") + name);
    } else {
        showOsd("Snapshot failed");
    }
}

void UIController::snapshotToClipboard() {
    if (!player_->isLoaded()) { showOsd("No frame to copy"); return; }
    int w = 0, h = 0, stride = 0;
    std::vector<uint8_t> bgr;
    if (!player_->getLastFrameBGR(w, h, stride, bgr) || w <= 0 || h <= 0) {
        showOsd("Snapshot failed"); return;
    }
    // Build a packed BITMAPINFOHEADER + pixels block (CF_DIB layout).
    // Negative biHeight = top-down rows to match how sws wrote them.
    BITMAPINFOHEADER bih = {};
    bih.biSize        = sizeof(bih);
    bih.biWidth       = w;
    bih.biHeight      = -h;
    bih.biPlanes      = 1;
    bih.biBitCount    = 24;
    bih.biCompression = BI_RGB;
    bih.biSizeImage   = (DWORD)bgr.size();

    HGLOBAL hdib = GlobalAlloc(GMEM_MOVEABLE, sizeof(bih) + bgr.size());
    if (!hdib) { showOsd("Clipboard alloc failed"); return; }
    if (void* p = GlobalLock(hdib)) {
        std::memcpy(p, &bih, sizeof(bih));
        std::memcpy((uint8_t*)p + sizeof(bih), bgr.data(), bgr.size());
        GlobalUnlock(hdib);
    }
    if (!OpenClipboard(nullptr)) {
        GlobalFree(hdib);
        showOsd("Clipboard busy"); return;
    }
    EmptyClipboard();
    // Ownership transfers to the clipboard on success. On failure we own
    // hdib and must free it ourselves.
    if (SetClipboardData(CF_DIB, hdib)) {
        snapshot_flash_ = 0.7f;
        showOsd("Copied to clipboard");
    } else {
        GlobalFree(hdib);
        showOsd("Clipboard write failed");
    }
    CloseClipboard();
}

namespace {

// Trim leading/trailing whitespace + surrounding angle brackets/quotes.
// The angle-bracket case matters because email clients + Slack often
// wrap pasted URLs in "<...>" or "\"...\"".
std::string trimForClipboard(std::string s) {
    auto is_ws = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == 0;
    };
    while (!s.empty() && is_ws((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && is_ws((unsigned char)s.back()))  s.pop_back();
    if (s.size() >= 2) {
        char f = s.front(), b = s.back();
        if ((f == '<' && b == '>') || (f == '"' && b == '"') ||
            (f == '\'' && b == '\'')) {
            s = s.substr(1, s.size() - 2);
        }
    }
    return s;
}

bool looksLikeUrl(const std::string& s) {
    // Cheap prefix check - avformat_open_input handles the actual protocol
    // negotiation, we just decide whether to skip the "does this file exist"
    // branch. Case-insensitive on the scheme.
    static const char* kSchemes[] = {
        "http://", "https://", "rtsp://", "rtmp://", "rtmps://",
        "ftp://", "sftp://", "mms://", "mmsh://", "udp://", "srt://", "file://"
    };
    if (s.size() < 6) return false;
    std::string lower = s.substr(0, 12);
    for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
    for (const char* sch : kSchemes) {
        size_t n = std::strlen(sch);
        if (lower.size() >= n && lower.compare(0, n, sch) == 0) return true;
    }
    return false;
}

} // namespace

void UIController::pasteAndPlay() {
    if (!OpenClipboard(nullptr)) { showOsd("Clipboard busy"); return; }

    std::string picked;

    // Prefer CF_HDROP (a real file-drop token from Explorer) since it's
    // unambiguous. Fall back to CF_UNICODETEXT which is what Ctrl+C on
    // a URL or path produces from browsers / editors / chat apps.
    if (HANDLE h = GetClipboardData(CF_HDROP)) {
        HDROP drop = (HDROP)h;
        UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        if (count >= 1) {
            wchar_t wbuf[MAX_PATH];
            if (DragQueryFileW(drop, 0, wbuf, MAX_PATH) > 0) {
                int u8len = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1,
                                                nullptr, 0, nullptr, nullptr);
                if (u8len > 1) {
                    std::string u8(u8len - 1, '\0');
                    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, u8.data(),
                                        u8len, nullptr, nullptr);
                    picked = u8;
                }
            }
        }
    }
    if (picked.empty()) {
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
            if (wchar_t* wtext = (wchar_t*)GlobalLock(h)) {
                int u8len = WideCharToMultiByte(CP_UTF8, 0, wtext, -1,
                                                nullptr, 0, nullptr, nullptr);
                if (u8len > 1) {
                    std::string u8(u8len - 1, '\0');
                    WideCharToMultiByte(CP_UTF8, 0, wtext, -1, u8.data(),
                                        u8len, nullptr, nullptr);
                    // Multi-line paste (e.g. copied a list): first non-empty
                    // line wins. Rare, but leaves behavior predictable.
                    size_t nl = u8.find_first_of("\r\n");
                    if (nl != std::string::npos) u8 = u8.substr(0, nl);
                    picked = u8;
                }
                GlobalUnlock(h);
            }
        }
    }
    CloseClipboard();

    picked = trimForClipboard(std::move(picked));
    if (picked.empty()) { showOsd("Clipboard is empty"); return; }

    if (looksLikeUrl(picked)) {
        // openAny routes YouTube page URLs through the yt-dlp resolver and
        // hands everything else straight to loadPath / avformat_open_input.
        if (isYoutubeUrl(picked)) {
            openAny(picked);
        } else {
            showOsd("Opening URL...", 1.2f);
            loadPath(picked);
        }
        return;
    }

    // Not a URL - assume file / folder path. Folders expand into the
    // playlist the same way drag-drop of a folder does.
    DWORD attrs = GetFileAttributesA(picked.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        // Might be a bare path without scheme that FFmpeg can still open
        // (e.g. a UNC path). Try loadPath anyway; it will OSD "Cannot open"
        // on failure so the user gets feedback either way.
        loadPath(picked);
        return;
    }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        enqueuePaths({ picked }, true);
        return;
    }
    loadPath(picked);
}

void UIController::closeCurrent() {
    if (!player_->isLoaded()) return;
    // Snapshot resume before unload so the user can pick up where they
    // left off if they re-open the same file later.
    snapshotCurrentResume();
    if (resume_dirty_) writeResumeIndex();
    player_->unload();
    subtitles_.clear();
    subtitle_cache_idx_     = 0;
    subtitle_tracks_.clear();
    subtitle_track_active_  = -1;
    loop_a_ = -1.0;
    loop_b_ = -1.0;
    chapters_.clear();
    bookmarks_current_.clear();
    // Not playing a YouTube-resolved stream anymore - drop the source URL
    // so the top-overlay quality pill disappears.
    current_yt_source_url_.clear();
    if (thumb_provider_) thumb_provider_->close();
    SDL_SetWindowTitle(window_, "Vela");
    showOsd("Closed");
    nudgeVisible();
}

void UIController::jumpToPlaylistIndex(size_t i) {
    if (i >= playlist_.size()) return;
    playlist_.setCurrent(i);
    loadPath(playlist_.current());
}

void UIController::revealInExplorer(const std::string& path) {
    if (path.empty()) { showOsd("No path"); return; }
    // /select,"<path>" opens the containing folder with the file highlighted.
    // Quote the path so folder names with spaces survive the command line.
    std::string args = "/select,\"" + path + "\"";
    HINSTANCE h = ShellExecuteA(nullptr, "open", "explorer.exe",
                                args.c_str(), nullptr, SW_SHOWNORMAL);
    // ShellExecute returns a value > 32 on success. Anything <= 32 is an
    // error code (per Microsoft docs).
    if ((INT_PTR)h <= 32) showOsd("Reveal failed");
}

void UIController::copyPathToClipboard(const std::string& path) {
    if (path.empty()) { showOsd("No path"); return; }
    // Widen the UTF-8 path to UTF-16 so non-ASCII file names survive the
    // clipboard round-trip. CF_UNICODETEXT is what modern Windows apps read.
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(),
                                   (int)path.size(), nullptr, 0);
    if (wlen <= 0) { showOsd("Copy failed"); return; }
    // +1 for the null terminator required by CF_UNICODETEXT.
    size_t bytes = (size_t)(wlen + 1) * sizeof(wchar_t);
    HGLOBAL hmem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hmem) { showOsd("Copy failed"); return; }
    if (wchar_t* p = (wchar_t*)GlobalLock(hmem)) {
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(), p, wlen);
        p[wlen] = L'\0';
        GlobalUnlock(hmem);
    }
    if (!OpenClipboard(nullptr)) {
        GlobalFree(hmem);
        showOsd("Copy failed"); return;
    }
    EmptyClipboard();
    if (SetClipboardData(CF_UNICODETEXT, hmem)) {
        showOsd("Path copied");
    } else {
        GlobalFree(hmem);
        showOsd("Copy failed");
    }
    CloseClipboard();
}

void UIController::playNextInPlaylist() {
    // Try forward through the queue, skipping any item that fails to open
    // (renamed / removed / permission denied). Bounded by the playlist
    // size so a fully-broken queue eventually stops instead of hammering.
    size_t tries = playlist_.size();
    while (tries-- > 0 && playlist_.next()) {
        if (loadPath(playlist_.current())) { nudgeVisible(); return; }
    }
    // Ran off the end. Repeat All wraps (and reshuffles when applicable).
    if (playlist_.repeatMode() == Playlist::RepeatMode::All
        && !playlist_.empty()) {
        playlist_.reshuffle();
        playlist_.wrapToStart();
        // Same bounded skip after the wrap.
        size_t tries2 = playlist_.size();
        while (tries2-- > 0) {
            if (loadPath(playlist_.current())) break;
            if (!playlist_.next()) break;
        }
    }
    nudgeVisible();
}

void UIController::playPrevInPlaylist() {
    if (playlist_.prev()) loadPath(playlist_.current());
    nudgeVisible();
}

void UIController::loadCompanionSubs(const std::string& video_path) {
    // Enumerate every sidecar (movie.ko.srt, movie.en.srt, movie.srt, ...)
    // so a picker can present them; load the first one so out-of-box the
    // user gets subs without having to open Settings.
    subtitle_tracks_       = enumerateCompanionSubtitles(video_path);
    subtitle_track_active_ = -1;
    subtitles_.clear();
    subtitle_cache_idx_    = 0;
    if (!subtitle_tracks_.empty()) {
        if (loadSubtitleSidecar(subtitle_tracks_.front().path)) {
            subtitle_track_active_ = 0;
        }
    }
}

bool UIController::loadSubtitleSidecar(const std::string& path) {
    // Shared with the drop-file branch of handleEvent. Reads the sidecar
    // via decodeSubtitleFileToUtf8 (which handles UTF-8 / CP949) and
    // picks a parser by lowercased extension.
    if (path.empty()) return false;
    std::string utf8 = decodeSubtitleFileToUtf8(path);
    std::string ext = path.size() >= 4 ? path.substr(path.size() - 4) : "";
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
    std::vector<SubtitleCue> cues;
    if (ext == ".srt") cues = parseSRT(utf8);
    else               cues = parseSMI(utf8);
    if (cues.empty()) return false;
    subtitles_ = std::move(cues);
    subtitle_cache_idx_ = 0;
    subtitles_enabled_ = true;
    return true;
}

void UIController::openSubtitleDialog() {
    if (!player_->isLoaded()) {
        showOsd("No video loaded");
        return;
    }
    OPENFILENAMEA ofn = {};
    char szFile[MAX_PATH] = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter =
        "Subtitle Files (*.srt;*.smi)\0*.srt;*.smi\0"
        "All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetOpenFileNameA(&ofn)) return;   // user cancelled
    std::string picked(szFile);
    if (loadSubtitleSidecar(picked)) {
        showOsd("Subtitle loaded");
    } else {
        showOsd("Subtitle load failed");
    }
    nudgeVisible();
}

const SubtitleCue* UIController::currentSubtitle(double t) {
    if (subtitles_.empty()) return nullptr;
    // Apply user-adjustable sync offset. Positive offset means subs are
    // shown LATER (subtract from t so the cue whose interval is future
    // wrt the video clock becomes current earlier vs. real time).
    t -= subtitle_offset_;
    // Cached index: try where we last were first, then walk forward, then
    // fall back to a bounded linear scan. Works for both natural playback
    // and small seeks; large seeks hit the fallback.
    auto in = [](const SubtitleCue& c, double x) { return x >= c.start && x < c.end; };
    if (subtitle_cache_idx_ >= 0
        && subtitle_cache_idx_ < (int)subtitles_.size()
        && in(subtitles_[subtitle_cache_idx_], t)) {
        return &subtitles_[subtitle_cache_idx_];
    }
    // Forward walk up to a few cues (natural playback).
    for (int k = 1; k <= 4; ++k) {
        int idx = subtitle_cache_idx_ + k;
        if (idx >= (int)subtitles_.size()) break;
        if (in(subtitles_[idx], t)) { subtitle_cache_idx_ = idx; return &subtitles_[idx]; }
        if (subtitles_[idx].start > t) return nullptr;  // in a gap
    }
    // Full scan (after seek).
    for (size_t k = 0; k < subtitles_.size(); ++k) {
        if (in(subtitles_[k], t)) { subtitle_cache_idx_ = (int)k; return &subtitles_[k]; }
    }
    return nullptr;
}

float UIController::subtitleBasePx(int W, int H) const {
    // Height-anchored ratios so subtitles scale up with the viewport.
    // The old width-based clamp locked Medium at 34 px above 900 px wide,
    // which meant a 1080p fullscreen showed the same tiny text as a small
    // windowed session - and 4K displays looked even worse. Anchoring off
    // H (video area height) tracks how "big" the picture feels to the user.
    float ratio = 0.045f;
    switch (subtitle_size_) {
        case SubtitleSize::Small:  ratio = 0.030f; break;
        case SubtitleSize::Medium: ratio = 0.045f; break;
        case SubtitleSize::Large:  ratio = 0.060f; break;
        case SubtitleSize::XL:     ratio = 0.075f; break;
    }
    float sz = (float)H * ratio;
    // Legibility floor for tiny windows (640x400 minimum).
    if (sz < 18.0f) sz = 18.0f;
    // Narrow-window squeeze so a 640x-tall window doesn't overshoot when
    // the height is also large (portrait-ish shapes).
    if (W < 900) sz *= 0.95f;
    if (W < 640) sz *= 0.85f;
    return sz;
}

void UIController::cycleSubtitleSize() {
    subtitle_size_ = (SubtitleSize)(((int)subtitle_size_ + 1) & 3);
    const char* n =
        subtitle_size_ == SubtitleSize::Small  ? "Sub size Small"
      : subtitle_size_ == SubtitleSize::Medium ? "Sub size Medium"
      : subtitle_size_ == SubtitleSize::Large  ? "Sub size Large" : "Sub size XL";
    showOsd(n);
    markConfigDirty();
}

void UIController::drawShortcutsPanel(int W, int H) {
    if (!show_shortcuts_) return;

    ImDrawList* bg = ImGui::GetForegroundDrawList();
    // Dim the whole window behind the modal.
    bg->AddRectFilled(ImVec2(0, 0), ImVec2((float)W, (float)H),
                      IM_COL32(0, 0, 0, 160));

    const float pw = std::min(720.0f, W - 80.0f);
    const float ph = std::min(560.0f, H - 80.0f);
    ImVec2 tl((W - pw) * 0.5f, (H - ph) * 0.5f);
    ImVec2 br(tl.x + pw, tl.y + ph);

    // Matched treatment with About / Info: soft shadow + accent hairline.
    bg->AddRectFilled(ImVec2(tl.x - 3, tl.y - 3), ImVec2(br.x + 3, br.y + 6),
                      IM_COL32(0, 0, 0, 90), 16.0f);
    bg->AddRectFilled(tl, br, IM_COL32(14, 18, 30, 246), 14.0f);
    bg->AddRect(tl, br, IM_COL32(96, 176, 246, 130), 14.0f, 0, 1.0f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                             | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar
                             | ImGuiWindowFlags_NoBackground
                             | ImGuiWindowFlags_NoBringToFrontOnFocus
                             | ImGuiWindowFlags_NoSavedSettings;
    ImGui::SetNextWindowPos(tl);
    ImGui::SetNextWindowSize(ImVec2(pw, ph));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 20));
    ImGui::Begin("##ShortcutsModal", nullptr, flags);

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.95f, 1.0f, 1.0f));
    ImGui::TextUnformatted("Keyboard shortcuts");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    struct Row { const char* key; const char* action; };
    struct Group { const char* title; std::vector<Row> rows; };
    const std::vector<Group> groups = {
        { "Playback", {
            { "Space",           "Play / Pause" },
            { "F  /  Enter  /  F11", "Fullscreen" },
            { "Esc",             "Exit fullscreen / close panel" },
            { "Double-click",    "Toggle fullscreen" },
            { "Middle click",    "Toggle fullscreen" },
            { "T",               "Always on top" },
            { "I",               "Media info" },
            { "Ctrl + H",        "Playback history" },
        }},
        { "Seek", {
            { "Left  /  Right",  "Back / forward 5 s" },
            { "PgUp  /  PgDn",   "Back / forward 30 s" },
            { "Mouse X1 / X2",   "Back / forward 5 s (side buttons)" },
            { "Home  /  End",    "Jump to start / near end" },
            { ",  /  .",         "Frame step" },
            { "A",               "A-B loop: set A / set B / clear" },
            { "Alt + Left / Right","Previous / next chapter" },
            { "Ctrl + M",        "Add bookmark at current time" },
        }},
        { "Sync", {
            { "Ctrl + Left / Right",       "Shift video by -/+ 30 ms" },
            { "Ctrl + 0",                  "Reset A/V sync" },
            { "Ctrl + Shift + Left/Right", "Subtitle sync +/- 100 ms" },
            { "Ctrl + Shift + 0",          "Reset subtitle sync" },
        }},
        { "Audio", {
            { "Up  /  Down",     "Volume +/- 5 %  (up to 200 %)" },
            { "Mouse wheel",     "Volume (over video)" },
            { "M",               "Mute" },
        }},
        { "Speed", {
            { "[",               "Slower" },
            { "]",               "Faster" },
            { "\\",              "Reset to 1.00x" },
        }},
        { "Files & tracks", {
            { "Ctrl + O",        "Open file..." },
            { "Ctrl + Shift + O","Open folder..." },
            { "Ctrl + V",        "Paste URL or path from clipboard and play" },
            { "Ctrl + W",        "Close current file" },
            { "Ctrl + S",        "Snapshot (PNG next to Vela.exe)" },
            { "Ctrl + Shift + S","Snapshot to clipboard" },
            { "0 - 9",           "Jump to 0 - 90 % of the file" },
            { "Delete",          "Remove item from playlist (panel open)" },
            { "Drop .srt / .smi","Load subtitles for current video" },
            { "Right-click",     "Context menu" },
            { "Wheel over seekbar","Seek +/- 5 s" },
            { "N  /  Ctrl + N",  "Next in playlist" },
            { "Ctrl + B",        "Previous in playlist" },
            { "P",               "Toggle playlist panel" },
            { "L",               "Cycle repeat: Off / One / All" },
            { "H",               "Shuffle" },
            { "S  /  G",         "Toggle subs  /  cycle sub size" },
            { "B",               "Toggle subtitle background box" },
            { "Alt + Up/Down/0", "Raise / lower / reset subtitle position" },
            { "Drop a file",     "Play immediately" },
        }},
        { "View", {
            { "Ctrl + R",        "Rotate 90 deg CW" },
            { "?  /  F1",        "Show this panel" },
        }},
    };

    // Two-column table per group.
    ImFont* font = ImGui::GetFont();
    const float col_w = 190.0f;
    for (const auto& g : groups) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.82f, 1.0f, 1.0f));
        ImGui::TextUnformatted(g.title);
        ImGui::PopStyleColor();
        for (const auto& row : g.rows) {
            float row_y = ImGui::GetCursorPosY();
            ImGui::SetCursorPosX(20.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.90f, 0.55f, 1.0f));
            ImGui::TextUnformatted(row.key);
            ImGui::PopStyleColor();
            ImGui::SameLine(20.0f + col_w);
            ImGui::TextUnformatted(row.action);
            (void)row_y;
        }
        ImGui::Spacing();
    }

    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("Press Esc or ? to close");

    ImGui::End();
    ImGui::PopStyleVar();
    (void)font;
}

void UIController::drawAboutPanel(int W, int H) {
    if (!show_about_) return;

    ImDrawList* bg = ImGui::GetForegroundDrawList();
    bg->AddRectFilled(ImVec2(0, 0), ImVec2((float)W, (float)H),
                      IM_COL32(0, 0, 0, 150));

    const float pw = std::min(480.0f, W - 80.0f);
    const float ph = 320.0f;
    ImVec2 tl((W - pw) * 0.5f, (H - ph) * 0.5f);
    ImVec2 br(tl.x + pw, tl.y + ph);

    // Soft shadow beneath the panel + subtle inner-glow border for lift.
    bg->AddRectFilled(ImVec2(tl.x - 3, tl.y - 3), ImVec2(br.x + 3, br.y + 6),
                      IM_COL32(0, 0, 0, 90), 16.0f);
    bg->AddRectFilled(tl, br, IM_COL32(14, 18, 30, 246), 14.0f);
    bg->AddRect(tl, br, IM_COL32(96, 176, 246, 140), 14.0f, 0, 1.0f);

    ImFont* font = ImGui::GetFont();
    float cx = tl.x + pw * 0.5f;

    // Wordmark with a matching accent underline so the About panel echoes
    // the splash treatment (same brand, same rhythm).
    {
        const char* wm = "VELA";
        float sz = 56.0f;
        ImVec2 d = font->CalcTextSizeA(sz, FLT_MAX, 0.0f, wm);
        float wm_y = tl.y + 40.0f;
        // Faint accent glow + crisp white
        bg->AddText(font, sz, ImVec2(cx - d.x * 0.5f + 0.5f, wm_y + 2.0f),
                    IM_COL32(0, 0, 0, 80), wm);
        bg->AddText(font, sz, ImVec2(cx - d.x * 0.5f, wm_y),
                    IM_COL32(96, 176, 246, 40), wm);
        bg->AddText(font, sz, ImVec2(cx - d.x * 0.5f, wm_y),
                    IM_COL32(244, 248, 255, 255), wm);
        // Accent underline
        float bar_w = d.x * 0.65f;
        float bar_y = wm_y + sz + 4.0f;
        bg->AddRectFilledMultiColor(
            ImVec2(cx - bar_w * 0.5f, bar_y),
            ImVec2(cx + bar_w * 0.5f, bar_y + 2.0f),
            IM_COL32( 64, 132, 216,   0),
            IM_COL32(148, 202, 255, 255),
            IM_COL32(148, 202, 255, 255),
            IM_COL32( 64, 132, 216,   0));
    }
    // Version
    {
        char v[48];
        std::snprintf(v, sizeof(v), "Version %s", VELA_VERSION);
        float sz = 14.0f;
        ImVec2 d = font->CalcTextSizeA(sz, FLT_MAX, 0.0f, v);
        bg->AddText(font, sz, ImVec2(cx - d.x * 0.5f, tl.y + 110.0f),
                    IM_COL32(160, 180, 220, 220), v);
    }
    // Stack
    {
        const char* s = "SDL3  .  Dear ImGui  .  FFmpeg";
        float sz = 14.0f;
        ImVec2 d = font->CalcTextSizeA(sz, FLT_MAX, 0.0f, s);
        bg->AddText(font, sz, ImVec2(cx - d.x * 0.5f, tl.y + 138.0f),
                    IM_COL32(130, 150, 190, 200), s);
    }
    // Credit / tagline
    {
        const char* t = "Focused, keyboard-first video playback.";
        float sz = 14.0f;
        ImVec2 d = font->CalcTextSizeA(sz, FLT_MAX, 0.0f, t);
        bg->AddText(font, sz, ImVec2(cx - d.x * 0.5f, tl.y + 180.0f),
                    IM_COL32(200, 210, 235, 235), t);
    }
    // Esc hint - matches the Shortcuts / Info modals.
    {
        const char* t = "Press Esc to close";
        float sz = 12.0f;
        ImVec2 d = font->CalcTextSizeA(sz, FLT_MAX, 0.0f, t);
        bg->AddText(font, sz, ImVec2(cx - d.x * 0.5f, tl.y + 210.0f),
                    IM_COL32(140, 150, 175, 180), t);
    }

    // Close button via ImGui window overlaid at the bottom.
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                             | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar
                             | ImGuiWindowFlags_NoBackground
                             | ImGuiWindowFlags_NoBringToFrontOnFocus
                             | ImGuiWindowFlags_NoSavedSettings;
    ImGui::SetNextWindowPos(ImVec2(tl.x, tl.y + ph - 70.0f));
    ImGui::SetNextWindowSize(ImVec2(pw, 60.0f));
    ImGui::Begin("##AboutClose", nullptr, flags);
    float bw = 120.0f;
    ImGui::SetCursorPosX((pw - bw) * 0.5f);
    if (Style::TextButton("Close", ImVec2(bw, 36), false)) {
        show_about_ = false;
    }
    ImGui::End();
}

void UIController::drawInfoPanel(int W, int H) {
    if (!show_info_) return;

    ImDrawList* bg = ImGui::GetForegroundDrawList();
    bg->AddRectFilled(ImVec2(0, 0), ImVec2((float)W, (float)H),
                      IM_COL32(0, 0, 0, 160));

    const float pw = std::min(560.0f, W - 80.0f);
    const float ph = std::min(440.0f, H - 80.0f);
    ImVec2 tl((W - pw) * 0.5f, (H - ph) * 0.5f);
    ImVec2 br(tl.x + pw, tl.y + ph);
    // Matched treatment with the About panel: soft shadow + accent hairline.
    bg->AddRectFilled(ImVec2(tl.x - 3, tl.y - 3), ImVec2(br.x + 3, br.y + 6),
                      IM_COL32(0, 0, 0, 90), 16.0f);
    bg->AddRectFilled(tl, br, IM_COL32(14, 18, 30, 246), 14.0f);
    bg->AddRect(tl, br, IM_COL32(96, 176, 246, 130), 14.0f, 0, 1.0f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                             | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar
                             | ImGuiWindowFlags_NoBackground
                             | ImGuiWindowFlags_NoBringToFrontOnFocus
                             | ImGuiWindowFlags_NoSavedSettings;
    ImGui::SetNextWindowPos(tl);
    ImGui::SetNextWindowSize(ImVec2(pw, ph));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 20));
    ImGui::Begin("##InfoModal", nullptr, flags);

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.95f, 1.0f, 1.0f));
    ImGui::TextUnformatted("Media info");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    if (!player_->isLoaded()) {
        ImGui::TextDisabled("No file loaded.");
    } else {
        auto row = [](const char* k, const std::string& v) {
            ImGui::SetCursorPosX(20.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.68f, 0.78f, 0.96f, 1.0f));
            ImGui::TextUnformatted(k);
            ImGui::PopStyleColor();
            ImGui::SameLine(200.0f);
            ImGui::TextUnformatted(v.c_str());
        };

        const std::string& path = player_->getFilename();
        row("File",         baseName(path.c_str()));
        row("Path",         path);

        char buf[128];
        std::snprintf(buf, sizeof(buf), "%d x %d",
                      player_->getVideoWidth(), player_->getVideoHeight());
        row("Resolution",   buf);

        const char* vc = player_->getVideoCodecName();
        row("Video codec",  (vc && *vc) ? vc : "-");

        double fps = player_->getVideoFrameRate();
        if (fps > 0.0) std::snprintf(buf, sizeof(buf), "%.3f fps", fps);
        else           std::snprintf(buf, sizeof(buf), "-");
        row("Frame rate",   buf);

        row("Decoder",      player_->isHardwareDecoding()
                              ? (std::string("HW  ") + player_->getHardwareDecoderName())
                              : std::string("Software"));

        int rot_intrinsic = player_->getIntrinsicRotation();
        std::snprintf(buf, sizeof(buf), "%d deg  (from container)", rot_intrinsic);
        row("Rotation",     buf);

        if (player_->hasAudio()) {
            const char* ac = player_->getAudioCodecName();
            row("Audio codec", (ac && *ac) ? ac : "-");
            std::snprintf(buf, sizeof(buf), "%d Hz  .  %d ch",
                          player_->getAudioSampleRate(),
                          player_->getAudioChannels());
            row("Audio",       buf);
        } else {
            row("Audio",       "-");
        }

        int64_t br_bits = player_->getFileBitrate();
        if (br_bits > 0) std::snprintf(buf, sizeof(buf), "%.0f kbps", br_bits / 1000.0);
        else             std::snprintf(buf, sizeof(buf), "-");
        row("Bit rate",     buf);

        double dur = player_->getDuration();
        int h = (int)dur / 3600;
        int m = ((int)dur % 3600) / 60;
        int s = (int)dur % 60;
        std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
        row("Duration",     buf);
    }

    // yt-dlp status - lets first-time users discover that Vela can play
    // YouTube URLs by pasting one, and shows install guidance when the
    // binary isn't on PATH (or dropped next to Vela.exe).
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::SetCursorPosX(20.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.68f, 0.78f, 0.96f, 1.0f));
    ImGui::TextUnformatted("YouTube");
    ImGui::PopStyleColor();
    ImGui::SameLine(200.0f);
    YtDlpProbeState yt_pst = yt_resolver_ ? yt_resolver_->probeState()
                                          : YtDlpProbeState::Missing;
    if (yt_pst == YtDlpProbeState::Probing) {
        ImGui::TextUnformatted("detecting...");
    } else if (yt_pst == YtDlpProbeState::Available) {
        std::string ver  = yt_resolver_->ytDlpVersion();
        std::string ppth = yt_resolver_->ytDlpPath();
        int         age  = yt_resolver_->ytDlpAgeDays();
        char line[256];
        if (!ver.empty() && age >= 0) {
            std::snprintf(line, sizeof(line), "yt-dlp %s  (%d days old)",
                          ver.c_str(), age);
        } else if (!ver.empty()) {
            std::snprintf(line, sizeof(line), "yt-dlp %s", ver.c_str());
        } else {
            std::snprintf(line, sizeof(line), "yt-dlp detected");
        }
        ImGui::TextUnformatted(line);
        if (age > 30) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 200.0f);
            ImGui::TextDisabled("Update: winget upgrade yt-dlp");
        }
        if (!ppth.empty()) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 200.0f);
            ImGui::TextDisabled("%s", ppth.c_str());
        }
    } else {
        ImGui::TextUnformatted("not installed");
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 200.0f);
        ImGui::TextDisabled("Install: winget install yt-dlp");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("Press Esc or I to close");

    ImGui::End();
    ImGui::PopStyleVar();
}

void UIController::drawHistoryPanel(int W, int H) {
    if (!show_history_) return;

    ImDrawList* bg = ImGui::GetForegroundDrawList();
    bg->AddRectFilled(ImVec2(0, 0), ImVec2((float)W, (float)H),
                      IM_COL32(0, 0, 0, 160));

    const float pw = std::min(680.0f, W - 60.0f);
    const float ph = std::min(560.0f, H - 80.0f);
    ImVec2 tl((W - pw) * 0.5f, (H - ph) * 0.5f);
    ImVec2 br(tl.x + pw, tl.y + ph);
    bg->AddRectFilled(ImVec2(tl.x - 3, tl.y - 3), ImVec2(br.x + 3, br.y + 6),
                      IM_COL32(0, 0, 0, 90), 16.0f);
    bg->AddRectFilled(tl, br, IM_COL32(14, 18, 30, 246), 14.0f);
    bg->AddRect(tl, br, IM_COL32(96, 176, 246, 130), 14.0f, 0, 1.0f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                             | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar
                             | ImGuiWindowFlags_NoBackground
                             | ImGuiWindowFlags_NoBringToFrontOnFocus
                             | ImGuiWindowFlags_NoSavedSettings;
    ImGui::SetNextWindowPos(tl);
    ImGui::SetNextWindowSize(ImVec2(pw, ph));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 20));
    ImGui::Begin("##HistoryModal", nullptr, flags);

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.95f, 1.0f, 1.0f));
    ImGui::TextUnformatted("Playback history");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    // Build a merged MRU list: resume_index_ first (files with a known
    // progress mark), then any recent_files_ that aren't already covered.
    // The resume entries already carry pos + duration; recent-only entries
    // get a bare basename row with no progress bar.
    struct Row {
        std::string path;
        double      pos      = -1.0;
        double      duration = -1.0;
    };
    std::vector<Row> rows;
    rows.reserve(resume_index_.size() + recent_files_.size());
    for (const auto& e : resume_index_) {
        Row r; r.path = e.path; r.pos = e.pos; r.duration = e.duration;
        rows.push_back(std::move(r));
    }
    for (const auto& rf : recent_files_) {
        bool covered = false;
        for (const auto& r : rows) {
            if (r.path == rf) { covered = true; break; }
        }
        if (!covered) { Row r; r.path = rf; rows.push_back(std::move(r)); }
    }

    if (rows.empty()) {
        ImGui::TextDisabled("No history yet - open a file to start.");
    } else {
        ImGui::TextDisabled("%d files - click a row to resume  .  x removes",
                            (int)rows.size());
        ImGui::Spacing();

        // Deferred actions - apply after the loop so mutations don't shift
        // ImGui IDs mid-frame.
        std::string open_path;
        std::string remove_path;
        bool        clear_all = false;

        ImGui::BeginChild("##hist_scroll",
                          ImVec2(0.0f, ph - 130.0f),
                          false,
                          ImGuiWindowFlags_HorizontalScrollbar);
        for (size_t i = 0; i < rows.size(); ++i) {
            const auto& r = rows[i];
            ImGui::PushID((int)i);
            // Basename row (click = resume).
            std::string label = baseName(r.path.c_str());
            if (label.empty()) label = r.path;
            if (ImGui::Selectable(label.c_str(), false,
                                  ImGuiSelectableFlags_None,
                                  ImVec2(pw - 100.0f, 0.0f))) {
                open_path = r.path;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.path.c_str());
            ImGui::SameLine(pw - 90.0f);
            if (ImGui::SmallButton("x")) remove_path = r.path;

            // Progress bar row (only when we have a meaningful duration).
            if (r.duration > 0.0 && r.pos >= 0.0) {
                float frac = (float)std::clamp(r.pos / r.duration, 0.0, 1.0);
                char pbuf[64];
                std::snprintf(pbuf, sizeof(pbuf), "%s  /  %s",
                              formatTime(r.pos).c_str(),
                              formatTime(r.duration).c_str());
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8.0f);
                ImGui::ProgressBar(frac, ImVec2(pw - 130.0f, 8.0f), pbuf);
            }
            ImGui::Spacing();
            ImGui::PopID();
        }
        ImGui::EndChild();

        ImGui::Spacing();
        if (ImGui::Button("Clear all history")) clear_all = true;

        // Apply deferred actions.
        if (!open_path.empty()) {
            show_history_ = false;
            loadPath(open_path);
        }
        if (!remove_path.empty()) {
            clearResume(remove_path);
            auto it = std::find(recent_files_.begin(),
                                recent_files_.end(), remove_path);
            if (it != recent_files_.end()) recent_files_.erase(it);
            markConfigDirty();
        }
        if (clear_all) {
            if (!resume_index_.empty()) {
                resume_index_.clear();
                resume_dirty_ = true;
            }
            if (!recent_files_.empty()) {
                recent_files_.clear();
                markConfigDirty();
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("Press Esc or Ctrl+H to close");

    ImGui::End();
    ImGui::PopStyleVar();
}

void UIController::drawSubtitles(int W, int H) {
    if (!subtitles_enabled_ || subtitles_.empty()) return;
    if (!player_->isLoaded()) return;
    const SubtitleCue* cue = currentSubtitle(player_->getCurrentTime());
    if (!cue) return;

    // Use the merged font; size selected by the user (S/M/L/XL) with a
    // narrow-window squeeze so it stays readable at 640 px.
    ImFont* font = ImGui::GetFont();
    float base_px = subtitleBasePx(W, H);

    // Split into physical lines and measure each.
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char c : cue->text) {
            if (c == '\n') { lines.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        if (!cur.empty()) lines.push_back(cur);
    }

    // Draw stacked from the bottom up, above the bottom overlay area.
    // subtitle_vpos_ shifts the block vertically as a fraction of H:
    // positive raises it, negative lowers it (clamped to keep the block
    // inside the picture area).
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    float line_h    = base_px * 1.15f;
    float bottom_pad = 120.0f;   // sit above the ~108px bottom overlay
    float vshift    = std::clamp(subtitle_vpos_, -0.15f, 0.60f) * (float)H;
    float block_h   = line_h * (float)lines.size();
    float y_top     = (float)H - bottom_pad - block_h - vshift;
    // Keep the block on-screen even at extreme nudges.
    if (y_top < 8.0f) y_top = 8.0f;
    if (y_top + block_h > (float)H - 8.0f) y_top = (float)H - 8.0f - block_h;

    // Optional background box: one rounded rect that spans the widest
    // line's width plus padding, sits behind ALL lines. Drawn first so
    // the outlined text lands on top.
    if (subtitle_bg_) {
        float widest = 0.0f;
        for (const auto& s : lines) {
            ImVec2 sz = font->CalcTextSizeA(base_px, FLT_MAX, 0.0f, s.c_str());
            if (sz.x > widest) widest = sz.x;
        }
        float pad_x = base_px * 0.5f;
        float pad_y = base_px * 0.20f;
        ImVec2 tl(((float)W - widest) * 0.5f - pad_x, y_top - pad_y);
        ImVec2 br(((float)W + widest) * 0.5f + pad_x,
                  y_top + block_h + pad_y);
        dl->AddRectFilled(tl, br, IM_COL32(0, 0, 0, 170), base_px * 0.20f);
    }

    float y = y_top;
    for (const auto& s : lines) {
        ImVec2 sz = font->CalcTextSizeA(base_px, FLT_MAX, 0.0f, s.c_str());
        float x = ((float)W - sz.x) * 0.5f;
        // Dark shadow / outline: draw the text 8x offset in dark, then once
        // in white on top. Cheap but readable over any content.
        ImU32 sh = IM_COL32(0, 0, 0, 220);
        ImU32 fg = IM_COL32(255, 255, 255, 245);
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                if (dx == 0 && dy == 0) continue;
                dl->AddText(font, base_px, ImVec2(x + dx, y + dy), sh, s.c_str());
            }
        }
        dl->AddText(font, base_px, ImVec2(x, y), fg, s.c_str());
        y += line_h;
    }
}

void UIController::seekPrevChapter() {
    if (!player_->isLoaded() || chapters_.empty()) {
        showOsd("No chapters", 1.4f);
        return;
    }
    double t = player_->getCurrentTime();
    // "Previous" jumps to the start of the current chapter if we're more
    // than ~1.5 s in - matches the way a music player's Prev button works.
    // Only skips to the chapter before that if we're already near its start.
    int target = 0;
    for (int i = (int)chapters_.size() - 1; i >= 0; --i) {
        if (chapters_[i].start <= t + 1e-3) {
            target = (t - chapters_[i].start > 1.5 || i == 0) ? i : i - 1;
            break;
        }
    }
    player_->seek(chapters_[target].start);
    char buf[192];
    if (!chapters_[target].title.empty()) {
        std::snprintf(buf, sizeof(buf), "Chapter: %s", chapters_[target].title.c_str());
    } else {
        std::snprintf(buf, sizeof(buf), "Chapter %d / %d",
                      target + 1, (int)chapters_.size());
    }
    showOsd(buf, 1.6f);
}

void UIController::seekNextChapter() {
    if (!player_->isLoaded() || chapters_.empty()) {
        showOsd("No chapters", 1.4f);
        return;
    }
    double t = player_->getCurrentTime();
    for (size_t i = 0; i < chapters_.size(); ++i) {
        if (chapters_[i].start > t + 0.25) {   // ignore floating-point noise near boundary
            player_->seek(chapters_[i].start);
            char buf[192];
            if (!chapters_[i].title.empty()) {
                std::snprintf(buf, sizeof(buf), "Chapter: %s", chapters_[i].title.c_str());
            } else {
                std::snprintf(buf, sizeof(buf), "Chapter %d / %d",
                              (int)i + 1, (int)chapters_.size());
            }
            showOsd(buf, 1.6f);
            return;
        }
    }
    showOsd("Last chapter", 1.4f);
}

void UIController::cycleABLoop() {
    if (!player_->isLoaded()) return;
    double t = player_->getCurrentTime();
    if (loop_a_ < 0.0 && loop_b_ < 0.0) {
        loop_a_ = t;
        char buf[64]; std::snprintf(buf, sizeof(buf), "Loop A: %s", formatTime(loop_a_).c_str());
        showOsd(buf);
    } else if (loop_a_ >= 0.0 && loop_b_ < 0.0) {
        if (t <= loop_a_ + 0.5) {
            showOsd("B must be after A");
            return;
        }
        loop_b_ = t;
        char buf[96]; std::snprintf(buf, sizeof(buf), "Loop A-B: %s - %s",
                                    formatTime(loop_a_).c_str(),
                                    formatTime(loop_b_).c_str());
        showOsd(buf);
    } else {
        loop_a_ = -1.0;
        loop_b_ = -1.0;
        showOsd("Loop cleared");
    }
}

void UIController::checkABLoop() {
    if (loop_a_ < 0.0 || loop_b_ < 0.0) return;
    if (!player_->isLoaded()) return;
    double t = player_->getCurrentTime();
    if (t >= loop_b_) {
        player_->seek(loop_a_);
    }
}

void UIController::handleNaturalEnd() {
    // Finishing a file clears its resume mark so re-opening starts at 0.
    if (!player_->getFilename().empty()) { clearResume(player_->getFilename()); }

    // Repeat One: replay the same file.
    if (playlist_.repeatMode() == Playlist::RepeatMode::One) {
        player_->seek(0.0);
        player_->play();
        return;
    }
    // Normal advance with bounded skip on broken items.
    size_t tries = playlist_.size();
    while (tries-- > 0 && playlist_.hasNext()) {
        playlist_.next();
        if (loadPath(playlist_.current())) return;
    }
    // Repeat All: wrap and try again (reshuffle if in shuffle mode).
    if (playlist_.repeatMode() == Playlist::RepeatMode::All) {
        if (playlist_.size() > 1) {
            playlist_.reshuffle();
            playlist_.wrapToStart();
            size_t tries2 = playlist_.size();
            while (tries2-- > 0) {
                if (loadPath(playlist_.current())) return;
                if (!playlist_.next()) return;
            }
        } else {
            player_->seek(0.0);
            player_->play();
        }
    }
    // else: just paused at end
}

void UIController::newFrame() {
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
}

void UIController::render(int W, int H) {
    ImGuiIO& io = ImGui::GetIO();
    float dt = io.DeltaTime;

    // Drain any completed YouTube URL resolutions. Runs BEFORE the load
    // check so a just-arrived direct URL flips `loaded` this same frame
    // and the splash gets replaced with the video immediately.
    if (yt_resolver_) {
        YoutubeResolver::Result yr;
        while (yt_resolver_->tryDrainResult(yr)) {
            if (yr.id != pending_yt_request_id_) continue;   // stale
            if (yr.ok) {
                // Use the resolved-URL loader so current_yt_source_url_
                // survives the load - the quality pill needs it, and a
                // subsequent quality change needs to re-resolve it.
                loadResolvedYoutubeUrl(yr.media_url);
            } else {
                if (!yt_resolver_->ytDlpAvailable()) {
                    showOsd("yt-dlp not found - install: winget install yt-dlp", 4.5f);
                } else {
                    std::string m = yr.message.empty()
                        ? std::string("resolve failed")
                        : yr.message;
                    showOsd(std::string("YouTube resolve failed: ") + m, 3.5f);
                }
            }
            pending_yt_request_id_ = 0;
            pending_yt_source_url_.clear();
        }
    }

    bool loaded = player_->isLoaded();

    // A-B loop: if the play head crossed the B mark, jump back to A. Runs
    // regardless of overlay state so the loop keeps working while hidden.
    if (loaded) checkABLoop();

    // Auto-hide: once a video is loaded, controls fade after 3 s of true idle
    // so the viewer can focus on the picture. Only real input (mouse motion,
    // key press, wheel, click-drag) resets the timer - a parked cursor over
    // the bar or a button does NOT count as activity. Splash stays visible.
    if (loaded) {
        bool moved         = (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f);
        bool item_active   = ImGui::IsAnyItemActive();               // click-drag in progress
        bool popup_open    = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup);
        bool panel_open    = show_playlist_;                          // keep visible while browsing tracks
        if (moved || item_active || popup_open || panel_open) {
            ui_visible_timer_ = 3.0f;
            cursor_idle_timer_ = 1.0f;
        }
        ui_visible_timer_ -= dt;
        cursor_idle_timer_ -= dt;
        overlay_target_ = (ui_visible_timer_ > 0.0f) ? 1.0f : 0.0f;
        // Cursor only auto-hides in fullscreen; windowed keeps it visible so
        // the user can still reach the title bar and other windows. The
        // cursor uses its own ~1 s timer (independent of the 3 s overlay
        // fade) so the pointer stops covering the picture quickly while the
        // seek bar / play button stay visible for a while longer.
        if (fullscreen_) {
            bool want_visible = (cursor_idle_timer_ > 0.0f);
            if (want_visible && !cursor_visible_) {
                SDL_ShowCursor();
                cursor_visible_ = true;
            } else if (!want_visible && cursor_visible_) {
                SDL_HideCursor();
                cursor_visible_ = false;
            }
        } else if (!cursor_visible_) {
            SDL_ShowCursor();
            cursor_visible_ = true;
        }
    } else {
        overlay_target_ = 1.0f;
        if (!cursor_visible_) {
            SDL_ShowCursor();
            cursor_visible_ = true;
        }
    }

    // Smooth fade toward target. Snap in fast on activity, fade out gently.
    float speed = (overlay_target_ > overlay_alpha_) ? 22.0f : 6.0f;
    float step = std::min(1.0f, dt * speed);
    overlay_alpha_ += (overlay_target_ - overlay_alpha_) * step;
    if (std::fabs(overlay_alpha_ - overlay_target_) < 0.01f) overlay_alpha_ = overlay_target_;

    // OSD decay
    for (auto& m : osd_) m.remaining -= dt;
    osd_.erase(std::remove_if(osd_.begin(), osd_.end(),
                              [](const OsdMsg& m){ return m.remaining <= 0.0f; }),
               osd_.end());

    // Snapshot flash decay
    if (snapshot_flash_ > 0.0f) {
        snapshot_flash_ -= dt * 3.0f;
        if (snapshot_flash_ < 0.0f) snapshot_flash_ = 0.0f;
    }

    // (Click play/pause now fires immediately in handleEvent; a follow-up
    // double-click undoes it. No deferred timer required.)

    // ---- Draw ----
    if (!loaded) {
        drawSplash(W, H);
    } else {
        if (overlay_alpha_ > 0.01f) {
            drawTopOverlay(W, H, overlay_alpha_);
            drawBottomOverlay(W, H, overlay_alpha_);
            // Seek-bar hover thumbnail sits above the overlay so it can
            // float above the cursor. Fully hidden when the overlay is
            // faded out (nothing to hover), while dragging, or when the
            // setting is off.
            drawSeekBarThumbnail(W, H, (float)std::max(player_->getDuration(), 0.1));
        }
        if (show_playlist_ && overlay_alpha_ > 0.01f) {
            drawPlaylistPanel(W, H, overlay_alpha_);
        }
    }

    // Subtitles sit above the overlay area but below the OSD/flash so nothing
    // covers them. Only draws when a cue is active.
    if (loaded) drawSubtitles(W, H);

    drawShortcutsPanel(W, H);
    drawAboutPanel(W, H);
    drawInfoPanel(W, H);
    drawHistoryPanel(W, H);

    // Right-click context menu: open on request, position at cursor,
    // reuse the Settings menu contents so both surfaces stay in sync.
    if (request_context_menu_) {
        ImGui::SetNextWindowPos(context_menu_pos_);
        ImGui::OpenPopup("##context_menu");
        request_context_menu_ = false;
    }
    if (ImGui::BeginPopup("##context_menu")) {
        drawMainMenuContents();
        ImGui::EndPopup();
    }

    drawOsd(W, H);
    drawFlash(W, H);

    ImGui::Render();
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer_);
}

// -------------- Drawing sub-routines --------------

void UIController::drawSplash(int W, int H) {
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                             | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar
                             | ImGuiWindowFlags_NoBackground
                             | ImGuiWindowFlags_NoBringToFrontOnFocus
                             | ImGuiWindowFlags_NoSavedSettings;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)W, (float)H));
    ImGui::Begin("##Splash", nullptr, flags);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Base gradient: cooler top, near-black bottom. Slightly darker than
    // before so the wordmark and accent underline pop harder.
    Style::GradientRect(dl, ImVec2(0, 0), ImVec2((float)W, (float)H),
                        IM_COL32(14, 18, 28, 255), IM_COL32(3, 4,  8, 255));
    // Decorative color washes - accent-tinted from the top-left, warmer purple
    // from the bottom-right for a subtle "spotlight" feel.
    float rmax = std::max((float)W, (float)H);
    dl->AddCircleFilled(ImVec2(0, 0),
                        rmax * 0.60f, IM_COL32( 60,  96, 190, 34), 96);
    dl->AddCircleFilled(ImVec2((float)W, (float)H),
                        rmax * 0.55f, IM_COL32(120,  70, 200, 26), 96);
    // A hairline horizontal accent glow through the vertical midline. This is
    // the "premium dark landing page" trick every SaaS marketing team ships.
    {
        float ay = H * 0.34f;
        dl->AddRectFilledMultiColor(
            ImVec2(0.0f,     ay),
            ImVec2((float)W, ay + 1.0f),
            IM_COL32( 96, 176, 246,  0),
            IM_COL32( 96, 176, 246, 55),
            IM_COL32( 96, 176, 246, 55),
            IM_COL32( 96, 176, 246,  0));
    }

    float cx = W * 0.5f;
    // Push the hero content up a hair so the recents list gets vertical air.
    float cy = H * 0.34f;
    ImFont* font = ImGui::GetFont();

    // --- Wordmark: large "VELA" with a gradient wash + subtle glow beneath ---
    const char* wordmark = "VELA";
    // Scale with window height so it never dominates or shrinks to a whisper.
    float ts = std::clamp((float)H * 0.13f, 56.0f, 108.0f);
    ImVec2 td = font->CalcTextSizeA(ts, FLT_MAX, 0.0f, wordmark);
    // Extra letter spacing for a mark-like feel (draw each glyph separately).
    float letter_gap = ts * 0.10f;
    float total_w = td.x + letter_gap * 3.0f;  // 3 gaps for 4 letters
    float wm_x = cx - total_w * 0.5f;
    float wm_y = cy - ts * 0.5f;
    {
        float x = wm_x;
        for (const char* p = wordmark; *p; ++p) {
            char one[2] = { *p, 0 };
            ImVec2 gd = font->CalcTextSizeA(ts, FLT_MAX, 0.0f, one);
            // Layered shadow: two low-alpha offsets so the wordmark reads
            // "lifted", not "underlined." Kept small so it can't feel cheesy.
            dl->AddText(font, ts, ImVec2(x + 1.5f, wm_y + 5.0f),
                        IM_COL32(0, 0, 0, 90),  one);
            dl->AddText(font, ts, ImVec2(x + 0.5f, wm_y + 2.5f),
                        IM_COL32(0, 0, 0, 60),  one);
            // Faint accent glow behind the crisp glyph so it feels tinted
            // rather than pure-white on video.
            dl->AddText(font, ts, ImVec2(x, wm_y + 1.0f),
                        IM_COL32( 96, 176, 246, 50), one);
            dl->AddText(font, ts, ImVec2(x, wm_y),
                        IM_COL32(244, 248, 255, 255), one);
            x += gd.x + letter_gap;
        }
    }

    // Accent underline: horizontal gradient bar under the wordmark, plus a
    // second thinner glow line for the "under-luminance" premium feel.
    float bar_y = wm_y + ts + 8.0f;
    float bar_w = total_w * 0.62f;
    float bar_h = 3.0f;
    dl->AddRectFilledMultiColor(
        ImVec2(cx - bar_w * 0.5f, bar_y),
        ImVec2(cx + bar_w * 0.5f, bar_y + bar_h),
        IM_COL32( 64, 132, 216,  0),
        IM_COL32(148, 202, 255, 255),
        IM_COL32(148, 202, 255, 255),
        IM_COL32( 64, 132, 216,  0));
    // Second, wider, low-alpha glow bar below - a bloom.
    dl->AddRectFilledMultiColor(
        ImVec2(cx - bar_w * 0.75f, bar_y + bar_h + 1.0f),
        ImVec2(cx + bar_w * 0.75f, bar_y + bar_h + 4.0f),
        IM_COL32( 96, 176, 246,   0),
        IM_COL32( 96, 176, 246,  90),
        IM_COL32( 96, 176, 246,  90),
        IM_COL32( 96, 176, 246,   0));

    // Tagline: what Vela is for. Slightly larger + brighter than the hints
    // so the reader's eye lands on it first after the wordmark.
    const char* tagline = "Focused, keyboard-first video playback";
    float tag_size = 17.0f;
    ImVec2 tag_dim = font->CalcTextSizeA(tag_size, FLT_MAX, 0.0f, tagline);
    float tag_y = bar_y + bar_h + 24.0f;
    dl->AddText(font, tag_size, ImVec2(cx - tag_dim.x * 0.5f, tag_y),
                IM_COL32(196, 210, 234, 235), tagline);

    // Hint row (small): common actions. Two lines so the trailing "? for
    // shortcuts" tip has room even at 640 px width.
    const char* subtitle = "Drop a video or image    Ctrl+O to open    Space to play";
    float ss = 13.0f;
    ImVec2 sd = font->CalcTextSizeA(ss, FLT_MAX, 0.0f, subtitle);
    float sub_y = tag_y + tag_size + 12.0f;
    dl->AddText(font, ss, ImVec2(cx - sd.x * 0.5f, sub_y),
                IM_COL32(140, 156, 185, 210), subtitle);

    const char* subtitle2 = "?  or  F1  for keyboard shortcuts";
    ImVec2 sd2 = font->CalcTextSizeA(ss, FLT_MAX, 0.0f, subtitle2);
    float sub2_y = sub_y + ss + 6.0f;
    dl->AddText(font, ss, ImVec2(cx - sd2.x * 0.5f, sub2_y),
                IM_COL32(118, 134, 165, 210), subtitle2);

    // "Open File" button.
    ImVec2 btn_size(240, 46);
    float btn_y = sub2_y + ss + 22.0f;
    ImGui::SetCursorScreenPos(ImVec2(cx - btn_size.x * 0.5f, btn_y));
    if (Style::TextButton("Open File...", btn_size, true)) {
        openFileDialog();
    }

    // Recent files list. Half-column width scales with the window so the
    // header, divider, and selectable rows all fit inside the 640 minimum.
    if (!recent_files_.empty()) {
        float top = btn_y + btn_size.y + 30.0f;
        float half_w = std::min(220.0f, W * 0.44f);
        float sel_w  = half_w * 2.0f;
        // Header: uppercase label with an accent dot on the left so it feels
        // like a section marker instead of a floating word.
        const char* label = "RECENT";
        float label_size = 12.0f;
        ImVec2 lsz = font->CalcTextSizeA(label_size, FLT_MAX, 0.0f, label);
        dl->AddCircleFilled(ImVec2(cx - half_w + 3.0f, top + lsz.y * 0.5f),
                            3.0f, IM_COL32(96, 176, 246, 200), 12);
        dl->AddText(font, label_size, ImVec2(cx - half_w + 12.0f, top),
                    IM_COL32(150, 174, 210, 230), label);
        // Divider: gradient hairline so the rule doesn't clip hard at either
        // end - matches the wordmark underline treatment.
        dl->AddRectFilledMultiColor(
            ImVec2(cx - half_w, top + lsz.y + 6.0f),
            ImVec2(cx + half_w, top + lsz.y + 7.0f),
            IM_COL32( 90, 108, 140,   0),
            IM_COL32( 90, 108, 140, 180),
            IM_COL32( 90, 108, 140, 180),
            IM_COL32( 90, 108, 140,   0));
        float rec_y   = top + lsz.y + 16.0f;
        float row_h   = 24.0f;
        // Cap recent rows so the list doesn't overflow the splash bottom
        // (bottom-right version footer sits at H - 24, we want breathing room).
        float bottom_limit = (float)H - 60.0f;
        size_t max_rows = 6;
        // Style push: transparent selectable so we can paint our own accent
        // hover state (a soft left-side bar) instead of the default filled
        // rect - reads more like a modern list row.
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.24f, 0.36f, 0.58f, 0.35f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0.30f, 0.48f, 0.78f, 0.55f));
        for (size_t i = 0;
             i < recent_files_.size() && i < max_rows && (rec_y + row_h) < bottom_limit;
             ++i) {
            const std::string& f = recent_files_[i];
            ImGui::SetCursorScreenPos(ImVec2(cx - half_w, rec_y));
            ImGui::PushID((int)i);
            bool clicked = ImGui::Selectable(baseName(f.c_str()), false, 0,
                                             ImVec2(sel_w, row_h));
            // Accent left-tab on hover so the row telegraphs which item is
            // about to launch.
            if (ImGui::IsItemHovered()) {
                ImVec2 rmin = ImGui::GetItemRectMin();
                ImVec2 rmax = ImGui::GetItemRectMax();
                dl->AddRectFilled(ImVec2(rmin.x - 6.0f, rmin.y + 3.0f),
                                  ImVec2(rmin.x - 3.0f, rmax.y - 3.0f),
                                  IM_COL32(96, 176, 246, 230), 1.5f);
            }
            if (clicked) loadPath(f);
            ImGui::PopID();
            rec_y += 28.0f;
        }
        ImGui::PopStyleColor(2);
    }

    // Version bottom-right. Single source of truth via VELA_VERSION. The
    // "v" prefix goes into a slightly brighter tone than the tech stack so
    // the release number reads first at a glance.
    char verhead[24];
    std::snprintf(verhead, sizeof(verhead), "v%s", VELA_VERSION);
    const char* stack = "SDL3  +  FFmpeg  +  Dear ImGui";
    float vf_size = 12.0f;
    ImVec2 vh = font->CalcTextSizeA(vf_size, FLT_MAX, 0.0f, verhead);
    ImVec2 vs = font->CalcTextSizeA(vf_size, FLT_MAX, 0.0f, stack);
    float sep = 12.0f;
    float total = vh.x + sep + vs.x;
    float base_x = W - total - 18.0f;
    dl->AddText(font, vf_size, ImVec2(base_x, H - 24.0f),
                IM_COL32(140, 156, 190, 240), verhead);
    dl->AddText(font, vf_size, ImVec2(base_x + vh.x + sep, H - 24.0f),
                IM_COL32( 90, 104, 132, 220), stack);

    ImGui::End();
}

void UIController::drawTopOverlay(int W, int H, float alpha) {
    const int overlay_h = 60;
    ImDrawList* bdl = ImGui::GetBackgroundDrawList();
    // Symmetric fade with the bottom overlay: strong scrim near the edge,
    // gentle taper into the video. Very slightly cool (blue-black) so the
    // fade harmonizes with the accent instead of a warm grey wash.
    ImU32 top_col = Style::Fade(IM_COL32(0, 0, 4, 205), alpha);
    ImU32 bot_col = Style::Fade(IM_COL32(0, 0, 4,   0), alpha);
    bdl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2((float)W, (float)overlay_h),
                                 top_col, top_col, bot_col, bot_col);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                             | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar
                             | ImGuiWindowFlags_NoBackground
                             | ImGuiWindowFlags_NoBringToFrontOnFocus
                             | ImGuiWindowFlags_NoSavedSettings;
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2((float)W, (float)overlay_h));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 8));
    ImGui::Begin("##TopOverlay", nullptr, flags);

    // Title
    const std::string& fn = player_->getFilename();
    if (!fn.empty()) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImFont* font = ImGui::GetFont();
        const char* name = baseName(fn.c_str());
        float sz = 18.0f;
        // Faint text-shadow so the filename stays legible even when the top
        // gradient thins out at the right side of the title.
        dl->AddText(font, sz, ImVec2(24.5f, 16.0f),
                    Style::Fade(IM_COL32(0, 0, 0, 140), alpha), name);
        dl->AddText(font, sz, ImVec2(24.0f, 14.0f),
                    Style::Fade(IM_COL32(240, 246, 255, 255), alpha), name);

        // Playlist position badge - accent-tinted so it visually pairs with
        // the accent seek fill and the AMP/rate pills below.
        if (playlist_.size() > 1) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%zu / %zu",
                          playlist_.index() + 1, playlist_.size());
            ImVec2 pd = font->CalcTextSizeA(13.0f, FLT_MAX, 0.0f, buf);
            float x = 24.0f + font->CalcTextSizeA(sz, FLT_MAX, 0.0f, name).x + 12.0f;
            ImVec2 badge_tl(x, 16.0f);
            ImVec2 badge_br(x + pd.x + 18.0f, 16.0f + pd.y + 8.0f);
            float br = (pd.y + 8.0f) * 0.5f;
            dl->AddRectFilled(badge_tl, badge_br,
                              Style::Fade(IM_COL32(64, 132, 216, 210), alpha), br);
            dl->AddLine(ImVec2(badge_tl.x + br * 0.6f, badge_tl.y + 0.5f),
                        ImVec2(badge_br.x - br * 0.6f, badge_tl.y + 0.5f),
                        Style::Fade(IM_COL32(255, 255, 255, 70), alpha), 1.0f);
            dl->AddText(font, 13.0f, ImVec2(x + 9.0f, 19.0f),
                        Style::Fade(IM_COL32(245, 250, 255, 255), alpha), buf);
        }
    }

    // Close (fullscreen exit) button, snug to the top-right corner.
    if (fullscreen_) {
        const float sz = 32.0f;
        const float margin = 12.0f;
        ImGui::SetCursorScreenPos(ImVec2((float)W - sz - margin, margin));
        if (Style::IconButton("close_fs", Style::Icon::Close, ImVec2(sz, sz),
                              "Exit fullscreen (Esc)", false, 1.0f, alpha)) {
            setFullscreen(false);
        }
    }

    // YouTube quality pill - right-aligned, drawn only while a YouTube
    // resolved stream is playing. Sits left of the fullscreen close
    // button when in fullscreen. Click opens a popup with the 5 quality
    // choices. Visual language matches the PLAYLIST N/M badge (rounded
    // pill, blue accent) so the top-overlay row reads as one system.
    if (!current_yt_source_url_.empty()) {
        ImFont* font = ImGui::GetFont();
        char pill_buf[24];
        std::snprintf(pill_buf, sizeof(pill_buf), "YT  %s",
                      youtubeQualityLabel(youtube_quality_));
        const float pill_font_sz = 13.0f;
        ImVec2 pd = font->CalcTextSizeA(pill_font_sz, FLT_MAX, 0.0f, pill_buf);
        const float pad_x  = 12.0f;
        const float pad_y  = 5.0f;
        const float pill_w = pd.x + pad_x * 2.0f;
        const float pill_h = pd.y + pad_y * 2.0f;
        // Reserve room for the fullscreen close button so the pill never
        // overlaps it. Right margin matches the close button's own margin
        // for visual balance.
        const float right_margin = 12.0f;
        const float close_reserve = fullscreen_ ? (32.0f + 8.0f) : 0.0f;
        float x = (float)W - right_margin - close_reserve - pill_w;
        float y = 16.0f;
        if (x < 8.0f) x = 8.0f;   // hard-guard against negatives at extreme widths
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::InvisibleButton("##yt_quality_pill", ImVec2(pill_w, pill_h));
        bool hovered = ImGui::IsItemHovered();
        bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        if (hovered) {
            ImGui::SetTooltip("YouTube quality  (click to change)");
        }
        if (clicked) {
            ImGui::OpenPopup("##yt_quality_popup");
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        // Slightly brighter blue on hover for affordance, matching the
        // hover tint IconButton uses elsewhere. Alpha rides the overlay
        // fade so the pill vanishes with the rest of the chrome.
        ImU32 bg = hovered
            ? Style::Fade(IM_COL32(88, 156, 232, 235), alpha)
            : Style::Fade(IM_COL32(64, 132, 216, 215), alpha);
        ImVec2 tl(x, y);
        ImVec2 br(x + pill_w, y + pill_h);
        float br_r = pill_h * 0.5f;
        dl->AddRectFilled(tl, br, bg, br_r);
        // Same hairline highlight the Badge helper uses for a "physical"
        // pill feel.
        dl->AddLine(ImVec2(tl.x + br_r * 0.6f, tl.y + 0.5f),
                    ImVec2(br.x - br_r * 0.6f, tl.y + 0.5f),
                    Style::Fade(IM_COL32(255, 255, 255, 70), alpha), 1.0f);
        dl->AddText(font, pill_font_sz, ImVec2(x + pad_x, y + pad_y),
                    Style::Fade(IM_COL32(245, 250, 255, 255), alpha), pill_buf);

        if (ImGui::BeginPopup("##yt_quality_popup")) {
            struct Entry { const char* label; YoutubeQuality q; };
            Entry opts[] = {
                {"Auto (best available)", YoutubeQuality::Auto},
                {"1080p",                 YoutubeQuality::Q1080},
                {"720p",                  YoutubeQuality::Q720},
                {"480p",                  YoutubeQuality::Q480},
                {"Audio only",            YoutubeQuality::AudioOnly},
            };
            for (auto& e : opts) {
                bool selected = (youtube_quality_ == e.q);
                if (ImGui::MenuItem(e.label, nullptr, selected)) {
                    youtube_quality_ = e.q;
                    markConfigDirty();
                    if (!current_yt_source_url_.empty()) {
                        // Re-resolve the same source at the new quality.
                        // Position is lost - accepted tradeoff, same as
                        // the Settings-menu path.
                        openAny(current_yt_source_url_);
                    }
                }
            }
            ImGui::EndPopup();
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
}

void UIController::drawBottomOverlay(int W, int H, float alpha) {
    const int overlay_h = 108;
    ImVec2 tl(0.0f, (float)(H - overlay_h));
    ImVec2 br((float)W, (float)H);

    ImDrawList* bdl = ImGui::GetBackgroundDrawList();
    // Two-band gradient: an upper very-faint scrim (~48 px) then a stronger
    // darkening across the actual control zone. Softer than the previous
    // single-stop 0->220 - reads more like a modern player's "gentle fade
    // into shadow" than a black bar bolted onto the video.
    float mid_y = (float)H - overlay_h * 0.55f;
    ImU32 top_col = Style::Fade(IM_COL32(0, 0, 4,   0), alpha);
    ImU32 mid_col = Style::Fade(IM_COL32(0, 0, 4, 140), alpha);
    ImU32 bot_col = Style::Fade(IM_COL32(0, 0, 4, 225), alpha);
    bdl->AddRectFilledMultiColor(tl, ImVec2(br.x, mid_y),
                                 top_col, top_col, mid_col, mid_col);
    bdl->AddRectFilledMultiColor(ImVec2(tl.x, mid_y), br,
                                 mid_col, mid_col, bot_col, bot_col);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                             | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar
                             | ImGuiWindowFlags_NoBackground
                             | ImGuiWindowFlags_NoBringToFrontOnFocus
                             | ImGuiWindowFlags_NoSavedSettings;
    ImGui::SetNextWindowPos(tl);
    ImGui::SetNextWindowSize(ImVec2((float)W, (float)overlay_h));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22, 12));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 4));
    ImGui::Begin("##BottomOverlay", nullptr, flags);

    // --- Row 1: seek bar + time ---
    float cur = (float)player_->getCurrentTime();
    float dur = (float)std::max(player_->getDuration(), 0.1);
    float seek_val = cur;

    float pb_rate = player_->getPlaybackRate();
    bool  show_rate = std::fabs(pb_rate - 1.0f) > 0.01f;
    char  rate_buf[16] = {0};
    if (show_rate) std::snprintf(rate_buf, sizeof(rate_buf), "%.2fx", pb_rate);
    std::string tstr = formatTime(cur) + "  /  " + formatTime(dur);
    ImVec2 tsz = ImGui::CalcTextSize(tstr.c_str());
    // Include the badge width so the seek bar doesn't jitter when speed
    // toggles - reserve room even when not shown, so layout stays stable
    // on the very first fractional-x resize.
    float rate_reserve = show_rate
        ? (ImGui::CalcTextSize(rate_buf).x + 16.0f + 8.0f)
        : 0.0f;
    float time_area = tsz.x + rate_reserve + 24.0f;
    float seek_w = (float)W - 44.0f - time_area;
    if (seek_w < 100.0f) seek_w = 100.0f;

    bool seek_held = false;
    bool seek_changed = Style::SeekBar("##seek", seek_val, 0.0f, dur, seek_w, 22.0f, &seek_held);
    // Remember the SeekBar's screen rect so the wheel handler can seek
    // instead of adjusting volume when the cursor is over it.
    {
        ImVec2 rmin = ImGui::GetItemRectMin();
        ImVec2 rmax = ImGui::GetItemRectMax();
        last_seekbar_rect_ = ImVec4(rmin.x, rmin.y, rmax.x, rmax.y);
    }
    // Draw chapter tick markers over the seek bar. A muted sky-blue post
    // per chapter start, skipping the file-start (0.0) which would sit
    // under the fill's left edge. When the cursor is over the seek bar
    // AND near a chapter tick, a small floating title is rendered above
    // the bar - independent of the hover-thumbnail feature so it works
    // even when thumbnails are disabled.
    if (dur > 0.0f && !chapters_.empty()) {
        ImVec2 rmin = ImGui::GetItemRectMin();
        ImVec2 rmax = ImGui::GetItemRectMax();
        ImDrawList* sdl_dl = ImGui::GetWindowDrawList();
        ImU32 tick_col = Style::Fade(IM_COL32(180, 220, 255, 210), alpha);
        for (const auto& ch : chapters_) {
            if (ch.start <= 0.05 || ch.start >= dur) continue;
            float x = rmin.x + (float)(ch.start / dur) * (rmax.x - rmin.x);
            sdl_dl->AddRectFilled(ImVec2(x - 1.0f, rmin.y - 2.0f),
                                  ImVec2(x + 1.0f, rmax.y + 2.0f),
                                  tick_col);
        }
        // Hover-based tooltip. Cursor must be over the seek bar rect and
        // within +/- 6 px of a chapter tick's x. Uses foreground draw list
        // so it lands above the seek fill.
        ImGuiIO& io = ImGui::GetIO();
        float mx = io.MousePos.x, my = io.MousePos.y;
        if (mx >= rmin.x && mx <= rmax.x && my >= rmin.y - 2.0f && my <= rmax.y + 12.0f) {
            const VideoPlayer::Chapter* hover_ch = nullptr;
            for (const auto& ch : chapters_) {
                if (ch.start <= 0.05 || ch.start >= dur) continue;
                float x = rmin.x + (float)(ch.start / dur) * (rmax.x - rmin.x);
                if (std::fabs(mx - x) < 6.0f) { hover_ch = &ch; break; }
            }
            if (hover_ch) {
                char label[256];
                if (!hover_ch->title.empty()) {
                    std::snprintf(label, sizeof(label), "%s  (%s)",
                                  hover_ch->title.c_str(),
                                  formatTime(hover_ch->start).c_str());
                } else {
                    std::snprintf(label, sizeof(label), "Chapter at %s",
                                  formatTime(hover_ch->start).c_str());
                }
                ImDrawList* fg = ImGui::GetForegroundDrawList();
                ImVec2 tsz = ImGui::CalcTextSize(label);
                float pad = 6.0f;
                float tx = std::clamp(mx - tsz.x * 0.5f,
                                      rmin.x, rmax.x - tsz.x - 2.0f * pad);
                float ty = rmin.y - tsz.y - 10.0f;
                if (ty < 4.0f) ty = rmax.y + 6.0f;    // flip below if too high
                fg->AddRectFilled(ImVec2(tx, ty),
                                  ImVec2(tx + tsz.x + 2.0f * pad, ty + tsz.y + 6.0f),
                                  IM_COL32(20, 26, 36, 230), 4.0f);
                fg->AddText(ImVec2(tx + pad, ty + 3.0f),
                            IM_COL32(220, 232, 255, 255), label);
            }
        }
    }

    // Draw user bookmark markers - small amber diamonds SITTING BELOW the
    // track so they're clearly separated from chapter posts (sky-blue,
    // spanning the track height) and A-B markers (green / orange posts).
    // Hover reveals the timestamp.
    if (dur > 0.0f && !bookmarks_current_.empty()) {
        ImVec2 rmin = ImGui::GetItemRectMin();
        ImVec2 rmax = ImGui::GetItemRectMax();
        ImDrawList* sdl_dl = ImGui::GetWindowDrawList();
        ImU32 mk_fill = Style::Fade(IM_COL32(255, 190, 100, 235), alpha);
        ImU32 mk_edge = Style::Fade(IM_COL32(120,  70,  20, 220), alpha);
        float diamond_y = rmax.y + 6.0f;
        float diamond_r = 4.5f;
        for (size_t i = 0; i < bookmarks_current_.size(); ++i) {
            double t = bookmarks_current_[i];
            if (t < 0.0 || t > dur) continue;
            float x = rmin.x + (float)(t / dur) * (rmax.x - rmin.x);
            ImVec2 pts[4] = {
                ImVec2(x, diamond_y - diamond_r),
                ImVec2(x + diamond_r, diamond_y),
                ImVec2(x, diamond_y + diamond_r),
                ImVec2(x - diamond_r, diamond_y),
            };
            sdl_dl->AddConvexPolyFilled(pts, 4, mk_fill);
            sdl_dl->AddPolyline(pts, 4, mk_edge, ImDrawFlags_Closed, 1.0f);
        }
        // Hover tooltip for the nearest bookmark within +/- 6 px.
        ImGuiIO& io = ImGui::GetIO();
        float mx = io.MousePos.x, my = io.MousePos.y;
        if (mx >= rmin.x && mx <= rmax.x
            && my >= diamond_y - diamond_r - 4.0f
            && my <= diamond_y + diamond_r + 4.0f) {
            int hit = -1;
            for (size_t i = 0; i < bookmarks_current_.size(); ++i) {
                double t = bookmarks_current_[i];
                if (t < 0.0 || t > dur) continue;
                float x = rmin.x + (float)(t / dur) * (rmax.x - rmin.x);
                if (std::fabs(mx - x) < 6.0f) { hit = (int)i; break; }
            }
            if (hit >= 0) {
                char label[128];
                std::snprintf(label, sizeof(label), "Bookmark  %s",
                              formatTime(bookmarks_current_[(size_t)hit]).c_str());
                ImDrawList* fg = ImGui::GetForegroundDrawList();
                ImVec2 tsz = ImGui::CalcTextSize(label);
                float pad = 6.0f;
                float tx = std::clamp(mx - tsz.x * 0.5f,
                                      rmin.x, rmax.x - tsz.x - 2.0f * pad);
                float ty = diamond_y + diamond_r + 6.0f;
                fg->AddRectFilled(ImVec2(tx, ty),
                                  ImVec2(tx + tsz.x + 2.0f * pad, ty + tsz.y + 6.0f),
                                  IM_COL32(24, 20, 12, 230), 4.0f);
                fg->AddText(ImVec2(tx + pad, ty + 3.0f),
                            IM_COL32(255, 220, 170, 255), label);
            }
        }
    }

    // Draw A-B loop markers over the seek bar (uses the button's rect).
    if (dur > 0.0f && (loop_a_ >= 0.0 || loop_b_ >= 0.0)) {
        ImVec2 rmin = ImGui::GetItemRectMin();
        ImVec2 rmax = ImGui::GetItemRectMax();
        ImDrawList* sdl_dl = ImGui::GetWindowDrawList();
        auto drawMark = [&](double t, ImU32 col, const char* label) {
            float x = rmin.x + (float)(t / dur) * (rmax.x - rmin.x);
            // Slight extension above/below the track so the markers read as
            // "posts" instead of blending into the track's own fill.
            sdl_dl->AddRectFilled(ImVec2(x - 1.5f, rmin.y - 2.0f),
                                  ImVec2(x + 1.5f, rmax.y + 2.0f),
                                  Style::Fade(col, alpha));
            // Labels below the track (rmax.y + 2) so they never collide
            // with the current-time text sitting to the right of the bar.
            sdl_dl->AddText(ImVec2(x - 4.0f, rmax.y + 3.0f),
                            Style::Fade(col, alpha), label);
        };
        // Use the palette's OK (green) and Warn (amber) - lifted from Style
        // so A / B stay consistent with anything else labelled "good" or
        // "attention" (AMP badge shares kWarn).
        if (loop_a_ >= 0.0) drawMark(loop_a_, IM_COL32(128, 220, 148, 255), "A");
        if (loop_b_ >= 0.0) drawMark(loop_b_, IM_COL32(255, 168,  74, 255), "B");
        // Shade the A-B span so the loop region is visible at a glance.
        if (loop_a_ >= 0.0 && loop_b_ >= 0.0) {
            float xa = rmin.x + (float)(loop_a_ / dur) * (rmax.x - rmin.x);
            float xb = rmin.x + (float)(loop_b_ / dur) * (rmax.x - rmin.x);
            sdl_dl->AddRectFilled(ImVec2(xa, rmin.y), ImVec2(xb, rmax.y),
                                  Style::Fade(IM_COL32(128, 220, 148, 60), alpha));
        }
    }
    if (seek_held && !seek_dragging_) {
        // Drag started: remember play state and pause so audio doesn't tear
        // and the clock doesn't fight the drag direction.
        seek_dragging_ = true;
        seek_resume_on_release_ = player_->isPlaying();
        if (seek_resume_on_release_) player_->pause();
        seek_pending_val_ = seek_val;
        seek_last_issue_ticks_ = 0;   // force first issue below to happen immediately
        seek_last_issued_val_ = -1.0f;
    }
    if (seek_dragging_) {
        // Rate-limited scrubbing: track the freshest target locally, but
        // only actually call player_->seek() every ~120 ms of wallclock.
        // This keeps a live-preview feel without letting the demux thread
        // flush + re-decode the pipeline on every mouse pixel.
        if (seek_changed) seek_pending_val_ = seek_val;
        Uint64 now_ticks = SDL_GetTicks();
        bool first_issue = (seek_last_issue_ticks_ == 0);
        bool time_ok     = (now_ticks - seek_last_issue_ticks_) >= 120;
        bool val_moved   = std::fabs(seek_pending_val_ - seek_last_issued_val_) > 0.05f;
        if ((first_issue || time_ok) && val_moved) {
            player_->seek(seek_pending_val_);
            seek_last_issue_ticks_ = now_ticks;
            seek_last_issued_val_  = seek_pending_val_;
            nudgeVisible();
        }
    } else if (seek_changed) {
        // Not a drag - a single click on the track counts as one seek.
        player_->seek(seek_val);
        nudgeVisible();
    }
    if (!seek_held && seek_dragging_) {
        // Drag ended: always commit the final position, even if rate-limit
        // would have skipped this one, so the user lands exactly where they
        // let go rather than at the last throttled sample.
        if (std::fabs(seek_pending_val_ - seek_last_issued_val_) > 1e-4f) {
            player_->seek(seek_pending_val_);
        }
        seek_dragging_ = false;
        if (seek_resume_on_release_) player_->play();
        seek_resume_on_release_ = false;
    }
    ImGui::SameLine();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2.0f);
    if (show_rate) {
        // Small blue pill left of the time. Matches the "PLAYLIST N/M"
        // badge style so the two overlay states feel like siblings.
        Style::Badge(rate_buf,
                     Style::Fade(IM_COL32(60, 90, 160, 230), alpha),
                     Style::Fade(IM_COL32(220, 230, 250, 255), alpha));
        ImGui::SameLine();
    }
    ImGui::TextColored(ImVec4(0.85f, 0.90f, 1.0f, 1.0f), "%s", tstr.c_str());

    // --- Row 2: buttons ---
    // Unified 34x34 icon buttons, chunky 52x40 play button.
    const ImVec2 btn_sz(34, 34);
    const ImVec2 play_sz(52, 40);
    const float  slider_w   = 80.0f;
    const float  item_gap   = ImGui::GetStyle().ItemSpacing.x;
    // Right-side cluster: mute + volume slider + [snap, loop, playlist, settings, fullscreen].
    // Width includes all 6 icons + slider + 6 inter-item gaps + 1 leading gap after the spacer.
    const int    right_icon_count = 6;
    const float  right_group_w =
        right_icon_count * btn_sz.x + slider_w + 7.0f * item_gap;

    bool playing = player_->isPlaying();
    if (Style::IconButton("prev", Style::Icon::Prev, btn_sz,
                          "Previous track (Ctrl+B)",
                          false, 1.0f, alpha)) {
        playPrevInPlaylist();
    }
    ImGui::SameLine();
    if (Style::IconButton("seekback", Style::Icon::SeekBack, btn_sz,
                          "Back 5s (Left)", false, 1.0f, alpha)) {
        player_->seekRelative(-5.0);
        SEEK_OSD("- 5s");
        nudgeVisible();
    }
    ImGui::SameLine();
    // Play/Pause hero: filled accent pill, taller than the rest of the row,
    // vertically nudged so its extra height distributes above AND below the
    // 34 px baseline. The hand-drawn icon is drawn on top of the fill so the
    // button reads "this is THE control" - matches YouTube / Netflix / Spotify.
    float play_y = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(play_y - (play_sz.y - btn_sz.y) * 0.5f);
    {
        ImVec2 cursor = ImGui::GetCursorScreenPos();
        ImGui::PushID("playpause");
        bool clicked = ImGui::InvisibleButton("btn", play_sz);
        bool hovered = ImGui::IsItemHovered();
        bool held    = ImGui::IsItemActive();
        ImGui::PopID();

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 pbr(cursor.x + play_sz.x, cursor.y + play_sz.y);
        float rounding = play_sz.y * 0.5f;

        // Gradient body: accent-deep at the bottom rising into accent-light
        // at the top so the button looks lit-from-above.
        ImU32 g_top = held ? IM_COL32(64, 132, 216, 255)
                    : hovered ? IM_COL32(148, 202, 255, 255)
                              : IM_COL32(120, 190, 250, 255);
        ImU32 g_bot = held ? IM_COL32(40, 100, 180, 255)
                    : hovered ? IM_COL32( 88, 168, 236, 255)
                              : IM_COL32( 72, 148, 224, 255);
        g_top = Style::Fade(g_top, alpha);
        g_bot = Style::Fade(g_bot, alpha);
        dl->AddRectFilledMultiColor(cursor, pbr, g_top, g_top, g_bot, g_bot);
        // The multicolor helper doesn't honor rounding, so paint the corners
        // by re-drawing two smaller rounded rects at the mid-tone to hide the
        // sharp corners without a real rounded gradient path.
        ImU32 g_mid = held ? IM_COL32(52, 116, 198, 255)
                    : hovered ? IM_COL32(120, 190, 250, 255)
                              : IM_COL32(96, 176, 246, 255);
        g_mid = Style::Fade(g_mid, alpha);
        dl->AddRectFilled(cursor, pbr, g_mid, rounding);
        // Inset top highlight for a subtle "physical" feel.
        dl->AddLine(ImVec2(cursor.x + rounding, cursor.y + 0.5f),
                    ImVec2(pbr.x    - rounding, cursor.y + 0.5f),
                    Style::Fade(IM_COL32(255, 255, 255, 70), alpha), 1.0f);
        // Icon.
        ImVec2 center(cursor.x + play_sz.x * 0.5f, cursor.y + play_sz.y * 0.5f);
        float ic_size = std::min(play_sz.x, play_sz.y) * 0.42f;
        Style::DrawIcon(dl, center, ic_size,
                        playing ? Style::Icon::Pause : Style::Icon::Play,
                        Style::Fade(IM_COL32(255, 255, 255, 255), alpha));
        if (hovered) {
            ImGui::SetTooltip("%s", playing ? "Pause (Space)" : "Play (Space)");
        }
        if (clicked) {
            player_->togglePause();
            showOsd(playing ? "Pause" : "Play");
            nudgeVisible();
        }
    }
    ImGui::SetCursorPosY(play_y);
    ImGui::SameLine();
    if (Style::IconButton("seekfwd", Style::Icon::SeekForward, btn_sz,
                          "Forward 5s (Right)", false, 1.0f, alpha)) {
        player_->seekRelative(5.0);
        SEEK_OSD("+ 5s");
        nudgeVisible();
    }
    ImGui::SameLine();
    if (Style::IconButton("next", Style::Icon::Next, btn_sz,
                          "Next track (Ctrl+N)", false, 1.0f, alpha)) {
        playNextInPlaylist();
    }
    ImGui::SameLine();
    if (Style::IconButton("stop", Style::Icon::Stop, btn_sz,
                          "Stop", false, 1.0f, alpha)) {
        player_->pause();
        player_->seek(0.0);
        nudgeVisible();
    }

    // Right-align the volume/tools cluster. If the window is too narrow to fit
    // both groups the cluster continues inline instead of being clipped.
    ImGui::SameLine();
    float avail = ImGui::GetContentRegionAvail().x;
    if (avail > right_group_w) {
        ImGui::Dummy(ImVec2(avail - right_group_w, 0));
        ImGui::SameLine();
    }

    // Mute / volume
    bool muted = player_->isMuted();
    if (Style::IconButton("mute",
                          muted ? Style::Icon::Mute
                                : (player_->getVolume() < 0.4f ? Style::Icon::VolumeLow
                                                              : Style::Icon::Volume),
                          btn_sz, "Mute (M)", muted, 1.0f, alpha)) {
        player_->setMuted(!muted);
        nudgeVisible();
    }
    ImGui::SameLine();
    // Vertically center the slider against the 34px icon row.
    float slider_frame_h = ImGui::GetFrameHeight();
    float slider_y_off = (btn_sz.y - slider_frame_h) * 0.5f;
    float slider_row_y = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(slider_row_y + slider_y_off);
    ImGui::SetNextItemWidth(slider_w);
    float vol = player_->getVolume();
    if (ImGui::SliderFloat("##vol", &vol, 0.0f, 2.0f, "")) {
        player_->setVolume(vol);
        nudgeVisible();
    }
    // Boost-region indicator: the slider goes 0-200% but everything past
    // 100% is digital amplification (SDL_SetAudioStreamGain > 1.0), which
    // can distort. Draw a vertical marker at the 100% tick and highlight
    // the segment from 100% to the current position when the user has
    // pushed into the boost region so they know they're above unity.
    {
        ImVec2 s_min = ImGui::GetItemRectMin();
        ImVec2 s_max = ImGui::GetItemRectMax();
        ImDrawList* sdl_dl = ImGui::GetWindowDrawList();
        float mid_x = s_min.x + (s_max.x - s_min.x) * 0.5f;
        sdl_dl->AddLine(ImVec2(mid_x, s_min.y + 2.0f),
                        ImVec2(mid_x, s_max.y - 2.0f),
                        Style::Fade(IM_COL32(220, 230, 250, 200), alpha), 1.5f);
        // Tiny "100" label above the tick so users learn where unity is.
        ImFont* sfont = ImGui::GetFont();
        const char* lbl = "100";
        ImVec2 ld = sfont->CalcTextSizeA(9.0f, FLT_MAX, 0.0f, lbl);
        sdl_dl->AddText(sfont, 9.0f,
                        ImVec2(mid_x - ld.x * 0.5f, s_min.y - ld.y - 1.0f),
                        Style::Fade(IM_COL32(200, 210, 235, 190), alpha), lbl);
        if (vol > 1.0f) {
            float grab_x = s_min.x + (s_max.x - s_min.x) * (vol / 2.0f);
            sdl_dl->AddRectFilled(ImVec2(mid_x, s_max.y - 3.0f),
                                  ImVec2(grab_x, s_max.y),
                                  Style::Fade(IM_COL32(255, 145, 60, 230), alpha));
            // Small "AMP" pill above the slider when in boost region.
            char pct[16]; std::snprintf(pct, sizeof(pct), "AMP %d%%",
                                        (int)std::lround(vol * 100.0f));
            ImVec2 pd = sfont->CalcTextSizeA(10.0f, FLT_MAX, 0.0f, pct);
            float pad = 4.0f;
            ImVec2 ptl(grab_x - pd.x * 0.5f - pad, s_min.y - pd.y - 6.0f);
            ImVec2 pbr(grab_x + pd.x * 0.5f + pad, s_min.y - 3.0f);
            // Clamp horizontally so the pill doesn't clip past the slider
            // right edge at max volume.
            float overflow = pbr.x - s_max.x;
            if (overflow > 0.0f) { ptl.x -= overflow; pbr.x -= overflow; }
            sdl_dl->AddRectFilled(ptl, pbr,
                                  Style::Fade(IM_COL32(180, 90, 30, 235), alpha),
                                  (pbr.y - ptl.y) * 0.5f);
            sdl_dl->AddText(sfont, 10.0f,
                            ImVec2(ptl.x + pad, ptl.y + 1.0f),
                            Style::Fade(IM_COL32(255, 240, 220, 255), alpha), pct);
        }
    }
    ImGui::SetCursorPosY(slider_row_y);
    ImGui::SameLine();

    if (Style::IconButton("snap", Style::Icon::Snapshot, btn_sz,
                          "Snapshot (Ctrl+S)", false, 1.0f, alpha)) {
        takeSnapshot();
    }
    ImGui::SameLine();
    {
        Playlist::RepeatMode rm = playlist_.repeatMode();
        bool loop_active = (rm != Playlist::RepeatMode::None);
        if (Style::IconButton("loop", Style::Icon::Loop, btn_sz,
                              "Repeat (L cycles Off / One / All)",
                              loop_active, 1.0f, alpha)) {
            playlist_.cycleRepeatMode();
            const char* n =
                playlist_.repeatMode() == Playlist::RepeatMode::None ? "Repeat off"
              : playlist_.repeatMode() == Playlist::RepeatMode::One  ? "Repeat one"
                                                                     : "Repeat all";
            showOsd(n);
            markConfigDirty();
            nudgeVisible();
        }
    }
    ImGui::SameLine();
    if (Style::IconButton("pl", Style::Icon::Playlist, btn_sz,
                          "Playlist (P)", show_playlist_, 1.0f, alpha)) {
        show_playlist_ = !show_playlist_;
        nudgeVisible();
    }
    ImGui::SameLine();
    if (Style::IconButton("menu", Style::Icon::Settings, btn_sz,
                          "Settings", false, 1.0f, alpha)) {
        ImGui::OpenPopup("main_menu");
    }
    ImGui::SameLine();
    if (Style::IconButton("fs",
                          fullscreen_ ? Style::Icon::FullscreenExit
                                      : Style::Icon::Fullscreen,
                          btn_sz, "Fullscreen (F / Enter)",
                          fullscreen_, 1.0f, alpha)) {
        setFullscreen(!fullscreen_);
    }

    // Popup menu attached to the settings button
    if (ImGui::BeginPopup("main_menu")) {
        drawMainMenuContents();
        ImGui::EndPopup();
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
}

void UIController::drawSeekBarThumbnail(int W, int H, float duration) {
    if (!thumbnail_preview_enabled_) return;
    if (!thumb_provider_) return;
    if (!player_->isLoaded()) return;
    // Never intrude while the user is actively dragging - the seek bar
    // already reflects the target and the popup would be visual noise.
    if (seek_dragging_) return;
    // No hit-testing surface this frame (bottom overlay hidden). The
    // rect is zeroed by callers when the overlay isn't drawn.
    const ImVec4& r = last_seekbar_rect_;
    if (r.z <= r.x || r.w <= r.y) return;

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mp = io.MousePos;
    if (mp.x < r.x || mp.x > r.z || mp.y < r.y || mp.y > r.w) return;

    if (duration <= 0.1f) return;
    float frac = (mp.x - r.x) / std::max(1.0f, r.z - r.x);
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    double t = (double)frac * (double)duration;

    ThumbnailProvider::Result res = thumb_provider_->requestAt(t);

    // Compute popup geometry - even without a texture yet, draw a small
    // "loading..." placeholder so the user sees feedback while the first
    // decode is in flight.
    const float pad = 4.0f;
    const float label_h = ImGui::GetFontSize() + 2.0f;
    int tw = res.tex ? res.w : 200;
    int th = res.tex ? res.h : 112;
    // Fall back to a 16:9 placeholder if we don't know the aspect yet.
    if (tw <= 0) tw = 200;
    if (th <= 0) th = 112;

    float box_w = (float)tw + pad * 2.0f;
    float box_h = (float)th + pad * 2.0f + label_h;
    float box_x = mp.x - box_w * 0.5f;
    float box_y = mp.y - box_h - 10.0f;

    // Clamp horizontally so the popup never runs off screen.
    if (box_x < 4.0f) box_x = 4.0f;
    if (box_x + box_w > (float)W - 4.0f) box_x = (float)W - 4.0f - box_w;
    // If the popup would sit above the top edge, flip it below the cursor.
    if (box_y < 4.0f) box_y = mp.y + 18.0f;
    if (box_y + box_h > (float)H - 4.0f) box_y = (float)H - 4.0f - box_h;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    // Soft shadow so the thumbnail popup feels lifted off the seek bar (same
    // treatment as OSD toasts).
    dl->AddRectFilled(ImVec2(box_x - 2, box_y + 3),
                      ImVec2(box_x + box_w + 2, box_y + box_h + 6),
                      IM_COL32(0, 0, 0, 90), 8.0f);
    dl->AddRectFilled(ImVec2(box_x, box_y),
                      ImVec2(box_x + box_w, box_y + box_h),
                      IM_COL32(16, 20, 30, 230), 7.0f);
    dl->AddRect(ImVec2(box_x, box_y),
                ImVec2(box_x + box_w, box_y + box_h),
                IM_COL32(96, 176, 246, 140), 7.0f, 0, 1.0f);

    ImVec2 img_min(box_x + pad, box_y + pad);
    ImVec2 img_max(img_min.x + (float)tw, img_min.y + (float)th);
    if (res.tex) {
        dl->AddImage((ImTextureID)(intptr_t)res.tex, img_min, img_max);
    } else {
        // Placeholder rectangle so the popup still looks intentional
        // during the first-frame cold miss.
        dl->AddRectFilled(img_min, img_max, IM_COL32(40, 44, 58, 255), 4.0f);
        const char* msg = "...";
        ImVec2 tsz = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2((img_min.x + img_max.x) * 0.5f - tsz.x * 0.5f,
                           (img_min.y + img_max.y) * 0.5f - tsz.y * 0.5f),
                    IM_COL32(200, 210, 230, 220), msg);
    }

    // Time label below the image.
    std::string tlabel = formatTime(t);
    ImVec2 lsz = ImGui::CalcTextSize(tlabel.c_str());
    float lx = box_x + (box_w - lsz.x) * 0.5f;
    float ly = img_max.y + 2.0f;
    dl->AddText(ImVec2(lx, ly), IM_COL32(220, 230, 250, 255), tlabel.c_str());
}

void UIController::drawMainMenuContents() {
    if (ImGui::MenuItem("Open File...", "Ctrl+O")) openFileDialog();
    if (ImGui::MenuItem("Open Folder...", "Ctrl+Shift+O")) openFolderAsPlaylist();
    if (ImGui::MenuItem("Paste URL / Path", "Ctrl+V")) pasteAndPlay();
    if (ImGui::BeginMenu("Open Recent", !recent_files_.empty())) {
        for (const auto& f : recent_files_) {
            if (ImGui::MenuItem(baseName(f.c_str()))) loadPath(f);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Clear")) { recent_files_.clear(); markConfigDirty(); }
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Save Playlist as M3U...", nullptr, false,
                        playlist_.size() > 0)) {
        std::string p = savePlaylistDialog();
        if (!p.empty()) savePlaylistM3U(p);
    }
    if (ImGui::MenuItem("Load Playlist (M3U)...")) {
        std::string p = openPlaylistDialog();
        if (!p.empty()) loadPlaylistM3U(p);
    }

    ImGui::Separator();

    if (ImGui::BeginMenu("Rotation")) {
        struct Entry { const char* label; RotationMode m; };
        Entry opts[] = {
            {"0 deg",   RotationMode::Deg0},
            {"90 deg CW", RotationMode::Deg90},
            {"180 deg", RotationMode::Deg180},
            {"270 deg CW", RotationMode::Deg270},
        };
        for (auto& e : opts) {
            if (ImGui::MenuItem(e.label, nullptr, rotation_ == e.m)) {
                rotation_ = e.m;
                showOsd(std::string("Rotation ") + e.label);
                markConfigDirty();
            }
        }
        ImGui::Separator();
        ImGui::TextDisabled("Ctrl+R cycles");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("YouTube Quality")) {
        struct Entry { const char* label; YoutubeQuality q; };
        Entry opts[] = {
            {"Auto (best available)", YoutubeQuality::Auto},
            {"1080p",                 YoutubeQuality::Q1080},
            {"720p",                  YoutubeQuality::Q720},
            {"480p",                  YoutubeQuality::Q480},
            {"Audio only",            YoutubeQuality::AudioOnly},
        };
        for (auto& e : opts) {
            bool selected = (youtube_quality_ == e.q);
            if (ImGui::MenuItem(e.label, nullptr, selected)) {
                youtube_quality_ = e.q;
                markConfigDirty();
                // If a YouTube-resolved video is currently playing, re-
                // resolve at the new quality. Position is lost - accepted
                // tradeoff, same as switching audio track mid-play.
                if (!current_yt_source_url_.empty()) {
                    openAny(current_yt_source_url_);
                } else {
                    showOsd(std::string("YouTube quality: ") + youtubeQualityLabel(e.q));
                }
            }
        }
        ImGui::Separator();
        ImGui::TextDisabled("Single-URL formats only");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Audio Track",
                         player_->isLoaded() && player_->getAudioTrackCount() > 0)) {
        int n   = player_->getAudioTrackCount();
        int cur = player_->getCurrentAudioTrackIndex();
        for (int i = 0; i < n; ++i) {
            std::string name = player_->getAudioTrackName(i);
            if (ImGui::MenuItem(name.c_str(), nullptr, cur == i)) {
                if (player_->setAudioTrack(i)) {
                    showOsd(std::string("Audio: ") + name);
                }
            }
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Subtitles")) {
        if (ImGui::MenuItem("Load Subtitle...", nullptr, false, player_->isLoaded())) {
            openSubtitleDialog();
        }
        if (ImGui::MenuItem("Enabled", "S", subtitles_enabled_)) {
            subtitles_enabled_ = !subtitles_enabled_;
            markConfigDirty();
        }
        // Track picker: only shown when the current video has more than
        // one sidecar next to it (movie.ko.srt + movie.en.srt + ...).
        // A single-track file already gets its subs auto-loaded so a
        // picker would just clutter the menu.
        if (subtitle_tracks_.size() > 1) {
            if (ImGui::BeginMenu("Track")) {
                for (size_t i = 0; i < subtitle_tracks_.size(); ++i) {
                    bool active = (int)i == subtitle_track_active_;
                    if (ImGui::MenuItem(subtitle_tracks_[i].label.c_str(),
                                        nullptr, active)) {
                        if (loadSubtitleSidecar(subtitle_tracks_[i].path)) {
                            subtitle_track_active_ = (int)i;
                            char buf[192];
                            std::snprintf(buf, sizeof(buf), "Subs: %s",
                                          subtitle_tracks_[i].label.c_str());
                            showOsd(buf, 1.6f);
                        } else {
                            showOsd("Subtitle load failed", 2.0f);
                        }
                    }
                }
                ImGui::EndMenu();
            }
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Size")) {
            struct Entry { const char* label; SubtitleSize s; };
            Entry opts[] = {
                {"Small",  SubtitleSize::Small},
                {"Medium", SubtitleSize::Medium},
                {"Large",  SubtitleSize::Large},
                {"XL",     SubtitleSize::XL},
            };
            for (auto& e : opts) {
                if (ImGui::MenuItem(e.label, nullptr, subtitle_size_ == e.s)) {
                    subtitle_size_ = e.s;
                    markConfigDirty();
                }
            }
            ImGui::Separator();
            ImGui::TextDisabled("G cycles");
            ImGui::EndMenu();
        }
        char sync_buf[64];
        std::snprintf(sync_buf, sizeof(sync_buf),
                      "Sync offset: %+d ms",
                      (int)std::lround(subtitle_offset_ * 1000.0));
        ImGui::MenuItem(sync_buf, nullptr, false, false);
        ImGui::TextDisabled("  Ctrl+Shift+Left/Right  .  Ctrl+Shift+0 reset");
        ImGui::Separator();
        if (ImGui::MenuItem("Background box", "B", subtitle_bg_)) {
            subtitle_bg_ = !subtitle_bg_;
            markConfigDirty();
        }
        char vpos_buf[64];
        std::snprintf(vpos_buf, sizeof(vpos_buf),
                      "Position: %+d%%",
                      (int)std::lround(subtitle_vpos_ * 100.0f));
        ImGui::MenuItem(vpos_buf, nullptr, false, false);
        ImGui::TextDisabled("  Alt+Up/Down  .  Alt+0 reset");
        ImGui::EndMenu();
    }

    ImGui::Separator();

    if (ImGui::BeginMenu("Playback Speed", player_->isLoaded())) {
        float cur = player_->getPlaybackRate();
        const float presets[] = { 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f };
        for (float r : presets) {
            char label[16]; std::snprintf(label, sizeof(label), "%.2fx", r);
            bool active = std::fabs(cur - r) < 0.01f;
            if (ImGui::MenuItem(label, nullptr, active)) {
                player_->setPlaybackRate(r);
                char buf[32]; std::snprintf(buf, sizeof(buf), "Speed %.2fx", r);
                showOsd(buf);
            }
        }
        ImGui::Separator();
        ImGui::TextDisabled("[ slower   ] faster   \\ reset");
        ImGui::EndMenu();
    }

    if (ImGui::MenuItem("Snapshot", "Ctrl+S", false, player_->isLoaded())) takeSnapshot();

    if (ImGui::BeginMenu("Bookmarks", player_->isLoaded())) {
        if (ImGui::MenuItem("Add bookmark at current time", "Ctrl+M", false,
                            player_->isLoaded())) addBookmark();
        if (bookmarks_current_.empty()) {
            ImGui::Separator();
            ImGui::TextDisabled("No bookmarks");
        } else {
            ImGui::Separator();
            // Snapshot the vector because iteration + mutation on click
            // would invalidate our indices. Apply the intended action at
            // end-of-menu.
            int click_seek   = -1;
            int click_remove = -1;
            bool click_clear = false;
            for (size_t i = 0; i < bookmarks_current_.size(); ++i) {
                char label[96];
                std::snprintf(label, sizeof(label), "%zu.  %s",
                              i + 1,
                              formatTime(bookmarks_current_[i]).c_str());
                if (ImGui::MenuItem(label)) click_seek = (int)i;
                ImGui::SameLine();
                ImGui::PushID((int)i);
                if (ImGui::SmallButton("x")) click_remove = (int)i;
                ImGui::PopID();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Clear all")) click_clear = true;

            if (click_seek >= 0
                && click_seek < (int)bookmarks_current_.size()) {
                player_->seek(bookmarks_current_[(size_t)click_seek]);
                nudgeVisible();
            } else if (click_remove >= 0) {
                removeBookmark((size_t)click_remove);
            } else if (click_clear) {
                clearBookmarks();
            }
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Repeat")) {
        struct Entry { const char* label; Playlist::RepeatMode m; };
        Entry opts[] = {
            {"Off", Playlist::RepeatMode::None},
            {"One", Playlist::RepeatMode::One},
            {"All", Playlist::RepeatMode::All},
        };
        for (auto& e : opts) {
            if (ImGui::MenuItem(e.label, nullptr, playlist_.repeatMode() == e.m)) {
                playlist_.setRepeatMode(e.m);
                markConfigDirty();
            }
        }
        ImGui::Separator();
        ImGui::TextDisabled("L cycles");
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Shuffle", "H", playlist_.shuffle())) {
        playlist_.setShuffle(!playlist_.shuffle());
        showOsd(playlist_.shuffle() ? "Shuffle on" : "Shuffle off");
        markConfigDirty();
    }

    if (ImGui::MenuItem("Show Playlist", "P", show_playlist_)) {
        show_playlist_ = !show_playlist_;
    }
    if (ImGui::MenuItem(fullscreen_ ? "Exit Fullscreen" : "Fullscreen",
                        "F / Enter", fullscreen_)) {
        setFullscreen(!fullscreen_);
    }
    if (ImGui::MenuItem("Always on Top", "T", always_on_top_)) {
        always_on_top_ = !always_on_top_;
        SDL_SetWindowAlwaysOnTop(window_, always_on_top_);
        showOsd(always_on_top_ ? "Always on top" : "Always on top off");
        markConfigDirty();
    }
    if (ImGui::MenuItem("Thumbnail preview on seek bar", nullptr,
                        thumbnail_preview_enabled_)) {
        thumbnail_preview_enabled_ = !thumbnail_preview_enabled_;
        showOsd(thumbnail_preview_enabled_
                    ? "Seek preview on"
                    : "Seek preview off");
        markConfigDirty();
    }

    ImGui::Separator();

    if (ImGui::MenuItem("Media Info", "I", false, player_->isLoaded())) {
        show_info_ = true; show_shortcuts_ = false; show_about_ = false;
        show_history_ = false;
    }
    if (ImGui::MenuItem("Playback History", "Ctrl+H")) {
        show_history_ = true;
        show_shortcuts_ = false; show_about_ = false; show_info_ = false;
    }
    if (ImGui::MenuItem("Keyboard Shortcuts", "?  /  F1")) {
        show_shortcuts_ = true;
        show_about_ = false;
        show_info_ = false;
        show_history_ = false;
    }
    if (ImGui::MenuItem("About Vela")) {
        show_about_ = true;
        show_shortcuts_ = false;
        show_info_ = false;
        show_history_ = false;
    }

    ImGui::Separator();

    if (ImGui::MenuItem("Exit")) {
        if (onExitRequested) onExitRequested();
    }
}

void UIController::drawPlaylistPanel(int W, int H, float alpha) {
    // Cap panel width so the video area still has room at the 640 minimum
    // window - a 340 px panel at 640 px width leaves 300 px for the movie,
    // which is unusable. Scale down when the window is narrow.
    const int panel_w = std::min(340, std::max(260, W - 300));
    ImVec2 tl((float)(W - panel_w), 60.0f);
    ImVec2 br((float)W, (float)(H - 120));

    ImDrawList* bdl = ImGui::GetBackgroundDrawList();
    // Slightly darker + more opaque background for better text contrast
    // over bright frames. Two-layer border: a soft outer shadow for lift,
    // an accent hairline inside so the panel feels "docked, but modern."
    bdl->AddRectFilled(ImVec2(tl.x - 2, tl.y - 2), ImVec2(br.x, br.y + 4),
                       Style::Fade(IM_COL32(0, 0, 0, 90), alpha), 14.0f);
    bdl->AddRectFilled(tl, br, Style::Fade(IM_COL32(10, 12, 18, 240), alpha), 12.0f);
    bdl->AddRect(tl, br, Style::Fade(IM_COL32(96, 176, 246, 130), alpha), 12.0f, 0, 1.0f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                             | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar
                             | ImGuiWindowFlags_NoBackground
                             | ImGuiWindowFlags_NoBringToFrontOnFocus
                             | ImGuiWindowFlags_NoSavedSettings;
    ImGui::SetNextWindowPos(tl);
    ImGui::SetNextWindowSize(ImVec2(br.x - tl.x, br.y - tl.y));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
    ImGui::Begin("##Playlist", nullptr, flags);

    ImGui::TextColored(ImVec4(0.75f, 0.85f, 1.0f, 1.0f), "PLAYLIST");
    if (playlist_.size() > 0) {
        ImGui::SameLine();
        char count[24];
        std::snprintf(count, sizeof(count), "  %zu", playlist_.size());
        ImGui::TextColored(ImVec4(0.55f, 0.60f, 0.75f, 1.0f), "%s", count);
    }
    const float close_sz = 26.0f;
    const float clear_w  = 56.0f;
    float rx = ImGui::GetContentRegionAvail().x - close_sz - clear_w - 8.0f;
    if (rx > 0) {
        ImGui::SameLine();
        ImGui::Dummy(ImVec2(rx, 0));
    }
    ImGui::SameLine();
    // Clear only makes sense when there IS something to clear.
    ImGui::BeginDisabled(playlist_.empty());
    if (Style::TextButton("Clear", ImVec2(clear_w, close_sz), false)) {
        playlist_.clear();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (Style::IconButton("plclose", Style::Icon::Close, ImVec2(close_sz, close_sz),
                          "Close (P / Esc)", false, 1.0f, alpha)) {
        show_playlist_ = false;
    }
    ImGui::Separator();

    // Deferred playlist mutations - never mutate mid-loop because ImGui
    // Selectable IDs are stable across the frame and we want the row height
    // math to stay consistent within one draw pass. Reveal-in-Explorer and
    // Copy-Path fire immediately from the popup (no playlist mutation), so
    // they don't need to be deferred.
    int    remove_idx        = -1;
    int    move_up_idx       = -1;
    int    move_down_idx     = -1;
    int    move_top_idx      = -1;
    int    move_bottom_idx   = -1;
    size_t play_idx          = (size_t)-1;

    // On narrow panels (below ~300 px inside width) up/down would eat the
    // filename column, so we hide them and leave only the X. The keyboard
    // still exposes reorder via drag on the Selectable in a future sprint.
    const bool show_reorder = (panel_w >= 300);

    ImGui::BeginChild("##pllist", ImVec2(0, 0), 0, ImGuiWindowFlags_None);
    const float row_h    = 24.0f;
    const float btn_sz   = 20.0f;
    const float gap_w    = 6.0f;
    const float btns_w   = show_reorder ? (btn_sz * 3.0f + gap_w * 2.0f)
                                        :  btn_sz;
    for (size_t i = 0; i < playlist_.size(); ++i) {
        bool is_cur = (i == playlist_.index());
        ImGui::PushID((int)i);
        char label[32];
        std::snprintf(label, sizeof(label), "%zu. ", i + 1);
        std::string s = std::string(label) + baseName(playlist_.items()[i].c_str());
        // Reserve space on the right for the action buttons.
        float avail_w = ImGui::GetContentRegionAvail().x;
        float sel_w   = std::max(60.0f, avail_w - btns_w - 12.0f);
        if (ImGui::Selectable(s.c_str(), is_cur, 0, ImVec2(sel_w, row_h))) {
            play_idx = i;
        }
        if (ImGui::IsItemHovered()) {
            // Full-path tooltip so long file names still tell the user which
            // file is which. Cheap - Selectable already gets us the hover.
            ImGui::SetTooltip("%s", playlist_.items()[i].c_str());
        }
        // Right-click context menu. BeginPopupContextItem uses the last
        // item (the Selectable above) as the trigger and the PushID/PopID
        // guarantees each row gets its own popup id. Reveal + Copy fire
        // immediately (no playlist mutation); the four mutating actions
        // record intent into the deferred *_idx variables applied below.
        if (ImGui::BeginPopupContextItem("pl_ctx")) {
            if (ImGui::MenuItem("Play now"))       play_idx        = i;
            ImGui::Separator();
            ImGui::BeginDisabled(i == 0);
            if (ImGui::MenuItem("Move to top"))    move_top_idx    = (int)i;
            ImGui::EndDisabled();
            ImGui::BeginDisabled(i + 1 >= playlist_.size());
            if (ImGui::MenuItem("Move to bottom")) move_bottom_idx = (int)i;
            ImGui::EndDisabled();
            ImGui::Separator();
            if (ImGui::MenuItem("Reveal in Explorer")) {
                revealInExplorer(playlist_.items()[i]);
            }
            if (ImGui::MenuItem("Copy path")) {
                copyPathToClipboard(playlist_.items()[i]);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Remove"))         remove_idx      = (int)i;
            ImGui::EndPopup();
        }
        if (show_reorder) {
            ImGui::SameLine();
            if (Style::TextButton("^", ImVec2(btn_sz, btn_sz), false)) move_up_idx   = (int)i;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Move up");
            ImGui::SameLine();
            if (Style::TextButton("v", ImVec2(btn_sz, btn_sz), false)) move_down_idx = (int)i;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Move down");
        }
        ImGui::SameLine();
        if (Style::TextButton("x", ImVec2(btn_sz, btn_sz), false)) remove_idx    = (int)i;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove");
        ImGui::PopID();
    }
    if (playlist_.empty()) {
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.7f, 1.0f),
                           "(Drop files or open a video to start)");
    }
    ImGui::EndChild();

    // Apply deferred mutations. Order matters: play before reorder/remove so
    // "click a row" always wins if the user happens to double-tap fast.
    if (play_idx != (size_t)-1) {
        jumpToPlaylistIndex(play_idx);
    }
    if (move_up_idx     >= 0) playlist_.moveUp((size_t)move_up_idx);
    if (move_down_idx   >= 0) playlist_.moveDown((size_t)move_down_idx);
    if (move_top_idx    >= 0) playlist_.moveToTop((size_t)move_top_idx);
    if (move_bottom_idx >= 0) playlist_.moveToBottom((size_t)move_bottom_idx);
    if (remove_idx    >= 0) {
        bool was_current = ((size_t)remove_idx == playlist_.index());
        playlist_.remove((size_t)remove_idx);
        // If we deleted the row the user is watching, jump to whatever
        // index_ now points at (the tail item if we deleted the last one,
        // otherwise the "next" logical item).
        if (was_current && !playlist_.empty()) {
            loadPath(playlist_.current());
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
}

void UIController::drawOsd(int W, int H) {
    if (osd_.empty()) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    float y = 84.0f;
    for (const auto& m : osd_) {
        // Fade both ends: quick appear (0.16 s), slower fade (0.30 s) so the
        // last toast doesn't blink out mid-glance. Slight upward slide on
        // appear is a modern-notification cliche because it works.
        float appear = std::min(1.0f, (m.total - m.remaining) / 0.16f);
        float fade   = std::min(1.0f, m.remaining / 0.30f);
        float alpha  = std::min(appear, fade);
        float slide  = (1.0f - appear) * 6.0f;   // px

        float sz = 20.0f;
        ImVec2 dim = font->CalcTextSizeA(sz, FLT_MAX, 0.0f, m.text.c_str());
        float pad_x = 18.0f;
        float pad_y = 9.0f;
        float x = W - dim.x - pad_x * 2 - 24.0f;
        ImVec2 tl(x, y + slide);
        ImVec2 br(x + dim.x + pad_x * 2, y + slide + dim.y + pad_y * 2);
        float rounding = (dim.y + pad_y * 2) * 0.5f;

        // Soft drop-shadow underneath the pill so it lifts off the video.
        // Two blurred-looking rectangles at decreasing alpha approximate the
        // Material "elevation-2" look without a real blur.
        dl->AddRectFilled(ImVec2(tl.x - 2, tl.y + 4),
                          ImVec2(br.x + 2, br.y + 6),
                          Style::Fade(IM_COL32(0, 0, 0, 80), alpha), rounding + 2);
        dl->AddRectFilled(ImVec2(tl.x - 1, tl.y + 2),
                          ImVec2(br.x + 1, br.y + 4),
                          Style::Fade(IM_COL32(0, 0, 0, 60), alpha), rounding + 1);
        // Pill body: near-black with a hairline highlight along the top.
        dl->AddRectFilled(tl, br,
                          Style::Fade(IM_COL32(20, 24, 36, 235), alpha),
                          rounding);
        dl->AddLine(ImVec2(tl.x + rounding * 0.7f, tl.y + 0.5f),
                    ImVec2(br.x - rounding * 0.7f, tl.y + 0.5f),
                    Style::Fade(IM_COL32(255, 255, 255, 30), alpha), 1.0f);
        // Accent-tinted hairline border - thinner than the previous 1.5 px
        // ring so it whispers rather than shouts.
        dl->AddRect(tl, br,
                    Style::Fade(IM_COL32(96, 176, 246, 140), alpha),
                    rounding, 0, 1.0f);
        dl->AddText(font, sz, ImVec2(x + pad_x, tl.y + pad_y),
                    Style::Fade(IM_COL32(242, 246, 252, 255), alpha),
                    m.text.c_str());
        y += dim.y + pad_y * 2 + 10.0f;
    }
}

void UIController::drawFlash(int W, int H) {
    if (snapshot_flash_ <= 0.0f) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    // Softer peak (was 200) and an inset gradient hint of a "camera flash"
    // frame - top+bottom go bright white, the mid stays a shade lower so
    // the flash reads as a photographic pulse, not a white-screen dropout.
    float k = std::min(1.0f, snapshot_flash_);
    int a  = (int)(k * 175);
    int am = (int)(k * 130);
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2((float)W, (float)H),
                                IM_COL32(255, 255, 255, a),
                                IM_COL32(255, 255, 255, a),
                                IM_COL32(255, 255, 255, am),
                                IM_COL32(255, 255, 255, am));
}

// -------------- Event handling --------------

void UIController::handleEvent(const SDL_Event& event) {
    ImGui_ImplSDL3_ProcessEvent(&event);
    ImGuiIO& io = ImGui::GetIO();

    if (event.type == SDL_EVENT_KEY_DOWN) {
        if (io.WantCaptureKeyboard) return;
        SDL_Keycode key = event.key.key;
        SDL_Scancode sc = event.key.scancode;
        SDL_Keymod mod = event.key.mod;

        // Auto-repeat is only useful for seek/volume nudges - holding those
        // keys should keep the action going. Everything else (Space, F, Ctrl
        // combos, ...) must fire once per press. Ctrl-modified Left/Right are
        // NOT holdable: they nudge A/V sync (30 ms) or subtitle sync (100 ms)
        // per press - repeating them at OS rate would silently rack up drift.
        if (event.key.repeat) {
            bool arrow_no_ctrl =
                ((key == SDLK_LEFT) || (key == SDLK_RIGHT)) && !(mod & SDL_KMOD_CTRL);
            bool holdable =
                arrow_no_ctrl ||
                (key == SDLK_PAGEUP) || (key == SDLK_PAGEDOWN) ||
                (key == SDLK_UP)     || (key == SDLK_DOWN);
            if (!holdable) return;
        }

        nudgeVisible();

        if (key == SDLK_SPACE) {
            if (player_->isLoaded()) {
                bool p = player_->isPlaying();
                player_->togglePause();
                showOsd(p ? "Pause" : "Play");
            }
            return;
        }
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER
            || sc == SDL_SCANCODE_F || key == SDLK_F11) {
            setFullscreen(!fullscreen_);
            return;
        }
        if (key == SDLK_ESCAPE) {
            if (show_shortcuts_) { show_shortcuts_ = false; return; }
            if (show_about_)     { show_about_    = false; return; }
            if (show_info_)      { show_info_     = false; return; }
            if (show_history_)   { show_history_  = false; return; }
            if (show_playlist_)  { show_playlist_ = false; return; }
            if (fullscreen_)     { setFullscreen(false);   return; }
            return;
        }
        // File info modal (I, no modifiers).
        if (sc == SDL_SCANCODE_I && !(mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
            show_info_ = !show_info_;
            if (show_info_) {
                show_shortcuts_ = false; show_about_ = false;
                show_history_ = false;
            }
            return;
        }
        // Playback history modal (Ctrl+H). Ctrl gates the branch so plain H
        // (shuffle) stays intact.
        if (sc == SDL_SCANCODE_H && (mod & SDL_KMOD_CTRL)) {
            show_history_ = !show_history_;
            if (show_history_) {
                show_shortcuts_ = false; show_about_ = false;
                show_info_ = false;
            }
            return;
        }
        // Shortcuts help: '?' (Shift+/) or F1.
        if (key == SDLK_F1
            || (key == SDLK_SLASH && (mod & SDL_KMOD_SHIFT))) {
            show_shortcuts_ = !show_shortcuts_;
            if (show_shortcuts_) {
                show_about_ = false; show_info_ = false; show_history_ = false;
            }
            return;
        }
        // Ctrl+Shift+Arrow = subtitle sync nudge (100 ms steps, clamp +/-10s).
        // Ctrl+Arrow (no Shift) = A/V sync nudge (30 ms steps).
        if (key == SDLK_LEFT && (mod & SDL_KMOD_CTRL) && (mod & SDL_KMOD_SHIFT)) {
            subtitle_offset_ = std::max(-10.0, subtitle_offset_ - 0.100);
            subtitle_cache_idx_ = 0;
            char buf[48]; std::snprintf(buf, sizeof(buf), "Sub sync %+d ms",
                                        (int)std::lround(subtitle_offset_ * 1000.0));
            showOsd(buf);
            markConfigDirty();
            return;
        }
        if (key == SDLK_RIGHT && (mod & SDL_KMOD_CTRL) && (mod & SDL_KMOD_SHIFT)) {
            subtitle_offset_ = std::min( 10.0, subtitle_offset_ + 0.100);
            subtitle_cache_idx_ = 0;
            char buf[48]; std::snprintf(buf, sizeof(buf), "Sub sync %+d ms",
                                        (int)std::lround(subtitle_offset_ * 1000.0));
            showOsd(buf);
            markConfigDirty();
            return;
        }
        // A/V sync nudge with Ctrl+Arrow. 30 ms steps, clamp handled by
        // VideoPlayer. Positive delays video (audio is heard late).
        if (key == SDLK_LEFT && (mod & SDL_KMOD_CTRL)) {
            if (player_->isLoaded()) {
                double v = player_->getAVSyncOffset() - 0.030;
                player_->setAVSyncOffset(v);
                char buf[48]; std::snprintf(buf, sizeof(buf), "A/V sync %+d ms",
                                            (int)std::lround(player_->getAVSyncOffset() * 1000.0));
                showOsd(buf);
                markConfigDirty();
            }
            return;
        }
        if (key == SDLK_RIGHT && (mod & SDL_KMOD_CTRL)) {
            if (player_->isLoaded()) {
                double v = player_->getAVSyncOffset() + 0.030;
                player_->setAVSyncOffset(v);
                char buf[48]; std::snprintf(buf, sizeof(buf), "A/V sync %+d ms",
                                            (int)std::lround(player_->getAVSyncOffset() * 1000.0));
                showOsd(buf);
                markConfigDirty();
            }
            return;
        }
        // Alt+Left / Alt+Right - previous / next container chapter. Falls
        // back to a "No chapters" OSD when the file has none. Checked
        // BEFORE the plain 5-second seek so Alt+arrow doesn't nudge time.
        if (key == SDLK_LEFT && (mod & SDL_KMOD_ALT)) {
            seekPrevChapter();
            return;
        }
        if (key == SDLK_RIGHT && (mod & SDL_KMOD_ALT)) {
            seekNextChapter();
            return;
        }
        if (key == SDLK_LEFT)  { if (player_->isLoaded()) { player_->seekRelative(-5.0); SEEK_OSD("- 5s"); } return; }
        if (key == SDLK_RIGHT) { if (player_->isLoaded()) { player_->seekRelative( 5.0); SEEK_OSD("+ 5s"); } return; }
        // Alt+Up / Alt+Down / Alt+0 - shift the subtitle line vertically
        // as a fraction of viewport height. Positive = raise (useful when
        // the video has burned-in captions eating the default position);
        // negative = lower. Reset with Alt+0. Persistent across launches.
        // Checked BEFORE the plain volume handlers so Alt+arrows don't
        // fall through to volume adjust.
        if (key == SDLK_UP && (mod & SDL_KMOD_ALT)) {
            subtitle_vpos_ = std::min(0.60f, subtitle_vpos_ + 0.03f);
            char buf[48]; std::snprintf(buf, sizeof(buf),
                                        "Sub position %+d%%",
                                        (int)std::lround(subtitle_vpos_ * 100.0f));
            showOsd(buf);
            markConfigDirty();
            return;
        }
        if (key == SDLK_DOWN && (mod & SDL_KMOD_ALT)) {
            subtitle_vpos_ = std::max(-0.15f, subtitle_vpos_ - 0.03f);
            char buf[48]; std::snprintf(buf, sizeof(buf),
                                        "Sub position %+d%%",
                                        (int)std::lround(subtitle_vpos_ * 100.0f));
            showOsd(buf);
            markConfigDirty();
            return;
        }
        if (key == SDLK_0 && (mod & SDL_KMOD_ALT) && !(mod & SDL_KMOD_CTRL)) {
            subtitle_vpos_ = 0.0f;
            showOsd("Sub position reset");
            markConfigDirty();
            return;
        }
        if (key == SDLK_UP) {
            float v = std::min(2.0f, player_->getVolume() + 0.05f);
            player_->setVolume(v);
            char buf[32]; std::snprintf(buf, sizeof(buf), "Volume %d%%", (int)(v * 100));
            showOsd(buf);
            return;
        }
        if (key == SDLK_DOWN) {
            float v = std::max(0.0f, player_->getVolume() - 0.05f);
            player_->setVolume(v);
            char buf[32]; std::snprintf(buf, sizeof(buf), "Volume %d%%", (int)(v * 100));
            showOsd(buf);
            return;
        }
        if (key == SDLK_HOME) { if (player_->isLoaded()) { player_->seek(0.0); showOsd("Start"); } return; }
        if (key == SDLK_END)  {
            if (player_->isLoaded()) {
                // Land just before the very end so we don't trip the EOF path
                // instantly; the user probably wants to see the last frames.
                double d = player_->getDuration();
                player_->seek(std::max(0.0, d - 1.0));
                showOsd("End");
            }
            return;
        }
        if (sc == SDL_SCANCODE_M) {
            player_->setMuted(!player_->isMuted());
            showOsd(player_->isMuted() ? "Muted" : "Unmuted");
            markConfigDirty();
            return;
        }
        if (sc == SDL_SCANCODE_O && (mod & SDL_KMOD_CTRL) && (mod & SDL_KMOD_SHIFT)) {
            openFolderAsPlaylist();
            return;
        }
        if (sc == SDL_SCANCODE_O && (mod & SDL_KMOD_CTRL)) { openFileDialog(); return; }
        // Ctrl+V: paste a URL / path / file-drop from the clipboard and play
        // immediately. Fires before Ctrl+Shift+V or Ctrl+Alt+V variations
        // exist, so no need to gate on !SHIFT.
        if (sc == SDL_SCANCODE_V && (mod & SDL_KMOD_CTRL)) {
            pasteAndPlay();
            return;
        }
        // Ctrl+Shift+S = snapshot to clipboard (checked before Ctrl+S file-save).
        if (sc == SDL_SCANCODE_S && (mod & SDL_KMOD_CTRL) && (mod & SDL_KMOD_SHIFT)) {
            snapshotToClipboard();
            return;
        }
        if (sc == SDL_SCANCODE_S && (mod & SDL_KMOD_CTRL)) { takeSnapshot(); return; }
        // Ctrl+W closes the currently-loaded file (playlist stays intact
        // so the user can pick something else without re-scanning).
        if (sc == SDL_SCANCODE_W && (mod & SDL_KMOD_CTRL)) {
            closeCurrent();
            return;
        }
        // Ctrl+M: add a user bookmark at the current playhead. Per-file,
        // persisted to vela.bookmarks. Duplicate protection: a Ctrl+M
        // within 1 s of an existing mark is a no-op.
        if (sc == SDL_SCANCODE_M && (mod & SDL_KMOD_CTRL)) {
            addBookmark();
            return;
        }
        // Number keys 0-9 (no modifiers): jump to N * 10 % of the file.
        // Ctrl+0 / Ctrl+Shift+0 are handled above for A/V and subtitle sync
        // resets, so we only fire when neither Ctrl nor Alt is held.
        if (!(mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))
            && key >= SDLK_0 && key <= SDLK_9) {
            if (player_->isLoaded()) {
                int n = (int)(key - SDLK_0);
                double d = player_->getDuration();
                if (d > 0.0) {
                    double target = d * (double)n / 10.0;
                    player_->seek(target);
                    char buf[48]; std::snprintf(buf, sizeof(buf), "Jump %d %%", n * 10);
                    showOsd(buf);
                }
            }
            return;
        }
        // Delete removes the current playlist entry when the playlist panel
        // is open. Guarded on show_playlist_ so it doesn't surprise-remove
        // anything when the user is just watching a file.
        if (key == SDLK_DELETE && show_playlist_ && !playlist_.empty()) {
            size_t cur = playlist_.index();
            bool was_current = true;
            playlist_.remove(cur);
            if (!playlist_.empty() && was_current) {
                loadPath(playlist_.current());
            } else if (playlist_.empty()) {
                closeCurrent();
            }
            return;
        }
        if (sc == SDL_SCANCODE_P && !(mod & SDL_KMOD_CTRL)) {
            show_playlist_ = !show_playlist_;
            return;
        }
        if (sc == SDL_SCANCODE_L && !(mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
            playlist_.cycleRepeatMode();
            const char* n =
                playlist_.repeatMode() == Playlist::RepeatMode::None ? "Repeat off"
              : playlist_.repeatMode() == Playlist::RepeatMode::One  ? "Repeat one"
                                                                     : "Repeat all";
            showOsd(n);
            markConfigDirty();
            return;
        }
        // Video rotation cycle. Ctrl+R avoids clashing with any letter-R
        // shortcut we might want later.
        if (sc == SDL_SCANCODE_R && (mod & SDL_KMOD_CTRL)) {
            rotation_ = (RotationMode)(((int)rotation_ + 1) & 3);
            const char* n =
                rotation_ == RotationMode::Deg0   ? "Rotation 0"
              : rotation_ == RotationMode::Deg90  ? "Rotation 90"
              : rotation_ == RotationMode::Deg180 ? "Rotation 180" : "Rotation 270";
            showOsd(n);
            markConfigDirty();
            return;
        }
        // Ctrl+= : snap the window's client area to the video aspect within
        // ~85 % of the current display's usable bounds. Explicitly excludes
        // Shift so a future Ctrl+Shift+= (or Ctrl++ on some layouts) stays
        // free. No auto-repeat - single press only.
        if (sc == SDL_SCANCODE_EQUALS
            && (mod & SDL_KMOD_CTRL) && !(mod & SDL_KMOD_SHIFT)) {
            snapWindowToVideo();
            return;
        }
        // Shuffle toggle (H, no modifiers).
        if (sc == SDL_SCANCODE_H && !(mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
            playlist_.setShuffle(!playlist_.shuffle());
            showOsd(playlist_.shuffle() ? "Shuffle on" : "Shuffle off");
            markConfigDirty();
            return;
        }
        // Always-on-top toggle (T, no modifiers).
        if (sc == SDL_SCANCODE_T && !(mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
            always_on_top_ = !always_on_top_;
            SDL_SetWindowAlwaysOnTop(window_, always_on_top_);
            showOsd(always_on_top_ ? "Always on top" : "Always on top off");
            markConfigDirty();
            return;
        }
        // Subtitle size cycle (G).
        if (sc == SDL_SCANCODE_G && !(mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
            cycleSubtitleSize();
            return;
        }
        // Subtitle background box toggle (plain B - Ctrl+B is prev-playlist
        // and remains gated on the CTRL branch below).
        if (sc == SDL_SCANCODE_B && !(mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
            subtitle_bg_ = !subtitle_bg_;
            showOsd(subtitle_bg_ ? "Sub background on" : "Sub background off");
            markConfigDirty();
            return;
        }
        if (key == SDLK_PAGEUP)   { if (player_->isLoaded()) { player_->seekRelative( 30.0); SEEK_OSD("+ 30s"); } return; }
        if (key == SDLK_PAGEDOWN) { if (player_->isLoaded()) { player_->seekRelative(-30.0); SEEK_OSD("- 30s"); } return; }
        if (key == SDLK_COMMA)    { if (player_->isLoaded()) { double fps = player_->getVideoFrameRate(); double step = (fps > 0.0) ? 1.0 / fps : 1.0 / 30.0; player_->stepFrame(-step); SEEK_OSD("< step"); } return; }
        if (key == SDLK_PERIOD)   { if (player_->isLoaded()) { double fps = player_->getVideoFrameRate(); double step = (fps > 0.0) ? 1.0 / fps : 1.0 / 30.0; player_->stepFrame( step); SEEK_OSD("step >"); } return; }
        // Playlist next/prev. Ctrl+N/Ctrl+B kept for muscle memory; plain N
        // is the shorter standard-player binding when no letter conflicts.
        if (key == SDLK_N && (mod & SDL_KMOD_CTRL))         { playNextInPlaylist(); return; }
        if (key == SDLK_B && (mod & SDL_KMOD_CTRL))         { playPrevInPlaylist(); return; }
        if (sc == SDL_SCANCODE_N && !(mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
            playNextInPlaylist(); return;
        }
        // Playback speed: [ slower, ] faster, \ reset. Step 0.25x, clamp 0.25-3.0.
        if (key == SDLK_LEFTBRACKET) {
            float r = std::max(0.25f, player_->getPlaybackRate() - 0.25f);
            player_->setPlaybackRate(r);
            char buf[32]; std::snprintf(buf, sizeof(buf), "Speed %.2fx", r);
            showOsd(buf);
            return;
        }
        if (key == SDLK_RIGHTBRACKET) {
            float r = std::min(3.0f, player_->getPlaybackRate() + 0.25f);
            player_->setPlaybackRate(r);
            char buf[32]; std::snprintf(buf, sizeof(buf), "Speed %.2fx", r);
            showOsd(buf);
            return;
        }
        if (key == SDLK_BACKSLASH) {
            player_->setPlaybackRate(1.0f);
            showOsd("Speed 1.00x");
            return;
        }
        // Ctrl+Shift+0 = reset subtitle sync (checked before Ctrl+0 A/V sync).
        if (key == SDLK_0 && (mod & SDL_KMOD_CTRL) && (mod & SDL_KMOD_SHIFT)) {
            subtitle_offset_ = 0.0;
            subtitle_cache_idx_ = 0;
            showOsd("Sub sync 0 ms");
            markConfigDirty();
            return;
        }
        // Reset A/V sync offset to zero. Ctrl+0 avoids clashing with the
        // aspect 1-5 number-key group.
        if (key == SDLK_0 && (mod & SDL_KMOD_CTRL)) {
            if (player_->isLoaded()) {
                player_->setAVSyncOffset(0.0);
                showOsd("A/V sync 0 ms");
                markConfigDirty();
            }
            return;
        }
        if (key == SDLK_A && !(mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
            cycleABLoop();
            return;
        }
        // Subtitle toggle (S alone; Ctrl+S is snapshot, handled below).
        if (sc == SDL_SCANCODE_S && !(mod & SDL_KMOD_CTRL)) {
            subtitles_enabled_ = !subtitles_enabled_;
            if (subtitles_.empty()) {
                showOsd("No subtitles found");
            } else {
                showOsd(subtitles_enabled_ ? "Subtitles on" : "Subtitles off");
            }
            markConfigDirty();
            return;
        }
    } else if (event.type == SDL_EVENT_DROP_FILE) {
        if (event.drop.data) {
            std::string p = event.drop.data;
            // Dropping a subtitle file onto a loaded video swaps in the
            // sidecar cues without touching playback state - matches VLC.
            if (isSubtitleFile(p) && player_->isLoaded()) {
                if (loadSubtitleSidecar(p)) {
                    char buf[128];
                    std::snprintf(buf, sizeof(buf), "Subs  %s",
                                  baseName(p.c_str()));
                    showOsd(buf, 2.0f);
                } else {
                    showOsd("Subtitle load failed");
                }
                nudgeVisible();
                return;
            }
            DWORD attrs = GetFileAttributesA(p.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                // Dropped a folder - scan it exactly like Ctrl+Shift+O.
                WIN32_FIND_DATAA fd{};
                std::string pattern = p + "\\*";
                HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
                std::vector<std::string> found;
                if (h != INVALID_HANDLE_VALUE) {
                    do {
                        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                        std::string full = p + "\\" + fd.cFileName;
                        if (isVideoFile(full)) found.push_back(full);
                    } while (FindNextFileA(h, &fd));
                    FindClose(h);
                }
                std::sort(found.begin(), found.end());
                if (found.empty()) {
                    showOsd("No videos in that folder");
                } else {
                    enqueuePaths(found, true);
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "Loaded  %zu  videos", found.size());
                    showOsd(buf);
                }
            } else {
                loadPath(p);
                showOsd(std::string("Playing  ") + baseName(p.c_str()));
            }
            nudgeVisible();
        }
    } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
        nudgeVisible();
    } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
        // Shift+Wheel adjusts playback speed +/-0.05 (mirrors [ / ] keys),
        // clamped to the same 0.25-3.0 range. Checked before the seekbar/volume
        // branches so it fires anywhere over the window.
        if ((SDL_GetModState() & SDL_KMOD_SHIFT) && player_->isLoaded()) {
            float step = (event.wheel.y > 0 ? 0.05f : -0.05f);
            float r = std::clamp(player_->getPlaybackRate() + step, 0.25f, 3.0f);
            player_->setPlaybackRate(r);
            char buf[32]; std::snprintf(buf, sizeof(buf), "Speed %.2fx", r);
            showOsd(buf);
            nudgeVisible();
            return;
        }
        // Wheel over the seek bar seeks; wheel elsewhere (over the video)
        // adjusts volume. The rect is captured each frame during render.
        {
            float mx = io.MousePos.x;
            float my = io.MousePos.y;
            const ImVec4& r = last_seekbar_rect_;
            bool over_seekbar = (r.z > r.x) && (mx >= r.x && mx <= r.z
                                             && my >= r.y && my <= r.w);
            if (over_seekbar && player_->isLoaded()) {
                double step = (event.wheel.y > 0 ? 5.0 : -5.0);
                player_->seekRelative(step);
                SEEK_OSD(step > 0 ? "+ 5s" : "- 5s");
                nudgeVisible();
                return;
            }
        }
        // Volume nudge from the wheel when the cursor is over the video area,
        // not over an ImGui widget. Standard player behavior.
        if (io.WantCaptureMouse) return;
        float step = (event.wheel.y > 0 ? 0.05f : -0.05f);
        float v = std::clamp(player_->getVolume() + step, 0.0f, 2.0f);
        player_->setVolume(v);
        if (player_->isMuted() && v > 0.0f) player_->setMuted(false);
        char buf[32]; std::snprintf(buf, sizeof(buf), "Volume %d%%", (int)(v * 100));
        showOsd(buf);
        nudgeVisible();
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        // Side buttons (X1 = back, X2 = forward) seek regardless of what
        // ImGui thinks the mouse is over. They aren't used by any widget,
        // and the user expects them to work while the cursor is over the
        // seek bar or playlist just as much as over the video area.
        if (event.button.button == SDL_BUTTON_X1) {
            if (player_->isLoaded()) {
                player_->seekRelative(-5.0);
                SEEK_OSD("- 5s");
                nudgeVisible();
            }
            return;
        }
        if (event.button.button == SDL_BUTTON_X2) {
            if (player_->isLoaded()) {
                player_->seekRelative( 5.0);
                SEEK_OSD("+ 5s");
                nudgeVisible();
            }
            return;
        }
        // Middle click (wheel button) toggles fullscreen. Bypass the ImGui
        // capture check for the same reason as X1/X2 - no widget uses the
        // middle button, and the user expects it to work anywhere on the
        // window. Middle click never participates in the left-click
        // double-click sequence, so no undo bookkeeping needed here.
        if (event.button.button == SDL_BUTTON_MIDDLE) {
            click_toggle_undoable_ = false;
            setFullscreen(!fullscreen_);
            nudgeVisible();
            return;
        }
        // Right click opens the context menu at the cursor. Anchor point
        // is captured here; the popup itself is opened during render().
        if (event.button.button == SDL_BUTTON_RIGHT && !io.WantCaptureMouse) {
            request_context_menu_ = true;
            context_menu_pos_ = ImVec2(event.button.x, event.button.y);
            nudgeVisible();
            return;
        }
        if (io.WantCaptureMouse) return;
        if (event.button.button == SDL_BUTTON_LEFT) {
            if (event.button.clicks >= 2) {
                // Double-click -> fullscreen. The first click of this pair
                // already toggled play/pause a moment ago; undo it here so
                // the net effect is JUST fullscreen. This "immediate + undo"
                // pattern is what makes single clicks feel snappy - waiting
                // out the full ~500 ms Windows double-click window before
                // toggling (the old behavior) felt laggy compared to Space.
                if (click_toggle_undoable_ && player_->isLoaded()) {
                    player_->togglePause();
                    if (!osd_.empty()) osd_.pop_back();  // drop the phantom "Pause"/"Play"
                }
                click_toggle_undoable_ = false;
                setFullscreen(!fullscreen_);
            } else if (player_->isLoaded()) {
                // Single click: toggle immediately. If this turns out to be
                // the first half of a double-click, the clicks>=2 branch
                // above will revert this before switching fullscreen.
                bool was_playing = player_->isPlaying();
                player_->togglePause();
                showOsd(was_playing ? "Pause" : "Play");
                click_toggle_undoable_ = true;
            }
        }
    }
}

// -------------- Persistent settings (vela.config) --------------

namespace {

std::string sidecarPath(const char* name) {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return name;
    std::string s(buf, n);
    size_t p = s.find_last_of("\\/");
    return (p == std::string::npos) ? std::string(name)
                                    : (s.substr(0, p) + "\\" + name);
}

std::string configPath() {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return "vela.config";
    std::string s(buf, n);
    size_t p = s.find_last_of("\\/");
    return (p == std::string::npos) ? "vela.config" : (s.substr(0, p) + "\\vela.config");
}

std::string strLower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

} // namespace

bool UIController::isSubtitleFile(const std::string& path) {
    static const char* kExts[] = { ".srt", ".smi" };
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = strLower(path.substr(dot));
    for (const char* e : kExts) if (ext == e) return true;
    return false;
}

bool UIController::isVideoFile(const std::string& path) {
    // Every extension Vela will pick up during a folder scan or Open
    // Folder. Videos AND stills - FFmpeg's image2 demuxer handles PNG /
    // JPEG / BMP / TIFF / WebP as single-frame "videos" and VideoPlayer
    // flags them via isStillImage() so they don't auto-advance in a
    // playlist. Keep in sync with the openFileDialogWin() filter below
    // and the OpenWithList block in resources/vela_reg_install.nsh.
    static const char* kExts[] = {
        // Video
        ".mp4", ".mkv", ".avi", ".mov", ".webm", ".wmv",
        ".flv", ".m4v", ".mpg", ".mpeg", ".ts",  ".3gp",
        // Still images
        ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp",
        ".tif", ".tiff",
    };
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = strLower(path.substr(dot));
    for (const char* e : kExts) if (ext == e) return true;
    return false;
}

void UIController::loadConfig() {
    std::string path = configPath();
    FILE* fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "rb") != 0 || !fp) return;
    char line[512];
    while (std::fgets(line, sizeof(line), fp)) {
        // Trim trailing CRLF
        size_t n = std::strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r' || line[n-1] == ' '))
            line[--n] = '\0';
        if (n == 0 || line[0] == '#') continue;
        char* eq = std::strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char* k = line;
        const char* v = eq + 1;
        if      (std::strcmp(k, "volume") == 0)          player_->setVolume((float)std::atof(v));
        else if (std::strcmp(k, "muted") == 0)           player_->setMuted(std::atoi(v) != 0);
        else if (std::strcmp(k, "subtitle_size") == 0) {
            int i = std::atoi(v);
            if (i < 0) i = 0; else if (i > 3) i = 3;
            subtitle_size_ = (SubtitleSize)i;
        }
        else if (std::strcmp(k, "subtitle_offset") == 0) subtitle_offset_ = std::atof(v);
        else if (std::strcmp(k, "subtitle_vpos") == 0) {
            float f = (float)std::atof(v);
            subtitle_vpos_ = std::clamp(f, -0.15f, 0.60f);
        }
        else if (std::strcmp(k, "subtitle_bg") == 0)     subtitle_bg_ = (std::atoi(v) != 0);
        else if (std::strcmp(k, "subtitles_enabled") == 0) subtitles_enabled_ = std::atoi(v) != 0;
        else if (std::strcmp(k, "always_on_top") == 0)   always_on_top_ = std::atoi(v) != 0;
        else if (std::strcmp(k, "repeat_mode") == 0) {
            int i = std::atoi(v);
            if (i < 0) i = 0; else if (i > 2) i = 2;
            playlist_.setRepeatMode((Playlist::RepeatMode)i);
        }
        else if (std::strcmp(k, "shuffle") == 0)         playlist_.setShuffle(std::atoi(v) != 0);
        else if (std::strcmp(k, "thumbnail_preview") == 0) thumbnail_preview_enabled_ = std::atoi(v) != 0;
        else if (std::strcmp(k, "youtube_quality") == 0) youtube_quality_ = youtubeQualityFromString(v);
        // A/V sync offset is deliberately NOT persisted - it's a per-file
        // transient (each file has its own encoder/mux quirks) and carrying
        // yesterday's -300 ms nudge into today's file would look like the
        // player just broke. Kept the key here so old config files parse
        // cleanly. Same rationale as rotation.
        else if (std::strcmp(k, "av_sync_offset") == 0) { /* ignore */ }
        // Rotation is intentionally NOT persisted - it's a per-file
        // transient. Persisting it caused landscape videos to appear
        // portrait after any earlier Ctrl+R press. Kept the case here
        // to eat the key silently so old config files don't fail parsing.
        else if (std::strcmp(k, "rotation") == 0) { /* ignore */ }
    }
    std::fclose(fp);
    if (always_on_top_) SDL_SetWindowAlwaysOnTop(window_, true);
}

void UIController::saveConfig() {
    std::string path = configPath();
    FILE* fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "wb") != 0 || !fp) return;
    std::fprintf(fp, "# Vela persistent settings\n");
    std::fprintf(fp, "volume=%.4f\n",           player_->getVolume());
    std::fprintf(fp, "muted=%d\n",              player_->isMuted() ? 1 : 0);
    std::fprintf(fp, "subtitle_size=%d\n",      (int)subtitle_size_);
    std::fprintf(fp, "subtitle_offset=%.4f\n",  subtitle_offset_);
    std::fprintf(fp, "subtitle_vpos=%.3f\n",    subtitle_vpos_);
    std::fprintf(fp, "subtitle_bg=%d\n",        subtitle_bg_ ? 1 : 0);
    std::fprintf(fp, "subtitles_enabled=%d\n",  subtitles_enabled_ ? 1 : 0);
    std::fprintf(fp, "always_on_top=%d\n",      always_on_top_ ? 1 : 0);
    std::fprintf(fp, "repeat_mode=%d\n",        (int)playlist_.repeatMode());
    std::fprintf(fp, "shuffle=%d\n",            playlist_.shuffle() ? 1 : 0);
    std::fprintf(fp, "thumbnail_preview=%d\n",  thumbnail_preview_enabled_ ? 1 : 0);
    std::fprintf(fp, "youtube_quality=%s\n",    youtubeQualityToString(youtube_quality_));
    // NOTE: rotation AND av_sync_offset are deliberately NOT saved -
    // both are per-file transients. See loadConfig for the ignore branches.
    std::fclose(fp);
    config_dirty_ = false;
}

void UIController::loadRecents() {
    std::string path = sidecarPath("vela.recent");
    FILE* fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "rb") != 0 || !fp) return;
    recent_files_.clear();
    char line[1024];
    while (std::fgets(line, sizeof(line), fp)) {
        size_t n = std::strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r' || line[n-1] == ' '))
            line[--n] = '\0';
        if (n == 0 || line[0] == '#') continue;
        recent_files_.emplace_back(line);
    }
    std::fclose(fp);
    if (recent_files_.size() > 12) recent_files_.resize(12);
}

void UIController::saveRecents() {
    std::string path = sidecarPath("vela.recent");
    FILE* fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "wb") != 0 || !fp) return;
    std::fprintf(fp, "# Vela recent files - most recent first\n");
    for (const auto& p : recent_files_) std::fprintf(fp, "%s\n", p.c_str());
    std::fclose(fp);
}

void UIController::applyStoredWindowGeometry() {
    std::string path = sidecarPath("vela.window");
    FILE* fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "rb") != 0 || !fp) return;
    int x = 0, y = 0, w = 0, h = 0, maximized = 0;
    bool have_pos = false, have_size = false;
    char line[256];
    while (std::fgets(line, sizeof(line), fp)) {
        char* eq = std::strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char* k = line;
        const char* v = eq + 1;
        if      (std::strcmp(k, "x") == 0) { x = std::atoi(v); have_pos = true; }
        else if (std::strcmp(k, "y") == 0) { y = std::atoi(v); have_pos = true; }
        else if (std::strcmp(k, "w") == 0) { w = std::atoi(v); have_size = true; }
        else if (std::strcmp(k, "h") == 0) { h = std::atoi(v); have_size = true; }
        else if (std::strcmp(k, "maximized") == 0) maximized = std::atoi(v);
    }
    std::fclose(fp);
    if (have_size && w >= 640 && h >= 400) {
        SDL_SetWindowSize(window_, w, h);
    }
    if (have_pos) {
        // Clamp so a monitor unplug can't strand the window off-screen.
        SDL_Rect bounds{};
        if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &bounds)) {
            if (x < bounds.x - 200 || x > bounds.x + bounds.w - 100) x = SDL_WINDOWPOS_CENTERED;
            if (y < bounds.y - 100 || y > bounds.y + bounds.h - 100) y = SDL_WINDOWPOS_CENTERED;
        }
        SDL_SetWindowPosition(window_, x, y);
    }
    if (maximized) SDL_MaximizeWindow(window_);
}

void UIController::snapWindowToVideo() {
    // Resize the window so its client area matches the video's post-rotation
    // aspect ratio, filling up to ~85 % of the current display's usable
    // bounds without going off-screen. Un-maximizes first if needed.
    // Silently no-ops in fullscreen (with a brief OSD) because SDL ignores
    // SDL_SetWindowSize on a fullscreen window and the resize would just be
    // stashed for the eventual restore anyway.
    if (!player_->isLoaded()) {
        showOsd("No video loaded");
        return;
    }
    int vw = player_->getVideoWidth();
    int vh = player_->getVideoHeight();
    if (vw <= 0 || vh <= 0) {
        showOsd("No video loaded");
        return;
    }
    if (fullscreen_
        || (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0) {
        showOsd("Not while fullscreen");
        return;
    }
    // Effective aspect after intrinsic + user rotation (matches the render
    // path in main.cpp so the picture fills the client area exactly).
    int total_deg = ((player_->getIntrinsicRotation()
                      + ((int)rotation_ * 90)) % 360 + 360) % 360;
    bool rot90 = (total_deg == 90 || total_deg == 270);
    int eff_w = rot90 ? vh : vw;
    int eff_h = rot90 ? vw : vh;
    // Fetch the display the window is currently on; fall back to primary.
    SDL_DisplayID disp = SDL_GetDisplayForWindow(window_);
    if (disp == 0) disp = SDL_GetPrimaryDisplay();
    SDL_Rect bounds{};
    if (!SDL_GetDisplayUsableBounds(disp, &bounds)) {
        if (!SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &bounds)) {
            showOsd("Cannot query display");
            return;
        }
    }
    // Un-maximize before resizing so the OS actually honors the new size
    // rather than snapping the frame right back to the work-area edges.
    if ((SDL_GetWindowFlags(window_) & SDL_WINDOW_MAXIMIZED) != 0) {
        SDL_RestoreWindow(window_);
    }
    const float fill = 0.85f;
    float max_w = (float)bounds.w * fill;
    float max_h = (float)bounds.h * fill;
    float src_aspect = (float)eff_w / (float)eff_h;
    // Fit the largest aspect-preserving rectangle into (max_w, max_h).
    float target_w = max_w;
    float target_h = target_w / src_aspect;
    if (target_h > max_h) {
        target_h = max_h;
        target_w = target_h * src_aspect;
    }
    int tw = (int)target_w;
    int th = (int)target_h;
    // Clamp to the declared minimum window size (project policy: 640x400).
    if (tw < 640) tw = 640;
    if (th < 400) th = 400;
    SDL_SetWindowSize(window_, tw, th);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Snap %dx%d", tw, th);
    showOsd(buf);
}

void UIController::captureWindowGeometry() {
    // Don't overwrite the saved geometry with fullscreen state - the user
    // usually wants their windowed position back on next launch.
    if (fullscreen_) return;
    int x = 0, y = 0, w = 0, h = 0;
    SDL_GetWindowPosition(window_, &x, &y);
    SDL_GetWindowSize(window_, &w, &h);
    bool maximized = (SDL_GetWindowFlags(window_) & SDL_WINDOW_MAXIMIZED) != 0;
    std::string path = sidecarPath("vela.window");
    FILE* fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "wb") != 0 || !fp) return;
    std::fprintf(fp, "x=%d\ny=%d\nw=%d\nh=%d\nmaximized=%d\n",
                 x, y, w, h, maximized ? 1 : 0);
    std::fclose(fp);
}

void UIController::loadLastPlaylist() {
    std::string path = sidecarPath("vela.session.m3u");
    FILE* fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "rb") != 0 || !fp) return;
    // Don't auto-play the last session (VLC-like restraint). Just prime
    // the playlist so N/Ctrl+N and the panel show the previous queue when
    // the user opens something new or re-plays a recent.
    char line[1024];
    while (std::fgets(line, sizeof(line), fp)) {
        size_t n = std::strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r' || line[n-1] == ' '))
            line[--n] = '\0';
        if (n == 0 || line[0] == '#') continue;
        playlist_.add(line);
    }
    std::fclose(fp);
}

void UIController::saveLastPlaylist() {
    std::string path = sidecarPath("vela.session.m3u");
    // Wipe the file if the playlist is empty so we don't strand yesterday's
    // list forever.
    if (playlist_.empty()) {
        std::remove(path.c_str());
        return;
    }
    FILE* fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "wb") != 0 || !fp) return;
    std::fprintf(fp, "#EXTM3U\n");
    for (const auto& p : playlist_.items()) std::fprintf(fp, "%s\n", p.c_str());
    std::fclose(fp);
}

// -------------- Open Folder + M3U --------------

std::string UIController::openFolderDialog() {
    // Trick: use the file dialog with a "select this folder" hint. We
    // rely on the user picking any file inside the folder and take the
    // parent directory. Cleaner Vista-style IFileDialog would be nicer
    // but this keeps the same code path as openFileDialog.
    OPENFILENAMEA ofn = {};
    char szFile[MAX_PATH] = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile   = szFile;
    ofn.nMaxFile    = sizeof(szFile);
    ofn.lpstrTitle  = "Pick any file in the folder to add all videos in it";
    ofn.lpstrFilter = "All Files\0*.*\0";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetOpenFileNameA(&ofn)) return {};
    std::string p = szFile;
    size_t slash = p.find_last_of("\\/");
    return (slash == std::string::npos) ? std::string() : p.substr(0, slash);
}

std::string UIController::savePlaylistDialog() {
    OPENFILENAMEA ofn = {};
    char szFile[MAX_PATH] = "playlist.m3u8";
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile   = szFile;
    ofn.nMaxFile    = sizeof(szFile);
    ofn.lpstrFilter = "M3U Playlist\0*.m3u;*.m3u8\0All Files\0*.*\0";
    ofn.lpstrDefExt = "m3u8";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetSaveFileNameA(&ofn)) return {};
    return std::string(szFile);
}

std::string UIController::openPlaylistDialog() {
    OPENFILENAMEA ofn = {};
    char szFile[MAX_PATH] = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile   = szFile;
    ofn.nMaxFile    = sizeof(szFile);
    ofn.lpstrFilter = "M3U Playlist\0*.m3u;*.m3u8\0All Files\0*.*\0";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetOpenFileNameA(&ofn)) return {};
    return std::string(szFile);
}

void UIController::openFolderAsPlaylist() {
    std::string dir = openFolderDialog();
    if (dir.empty()) return;
    WIN32_FIND_DATAA fd{};
    std::string pattern = dir + "\\*";
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        showOsd("Folder scan failed");
        return;
    }
    std::vector<std::string> found;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::string full = dir + "\\" + fd.cFileName;
        if (isVideoFile(full)) found.push_back(full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(found.begin(), found.end());
    if (found.empty()) {
        showOsd("No videos in that folder");
        return;
    }
    enqueuePaths(found, /*play_first=*/true);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Loaded  %zu  videos", found.size());
    showOsd(buf);
}

void UIController::savePlaylistM3U(const std::string& path) {
    FILE* fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "wb") != 0 || !fp) {
        showOsd("Save failed"); return;
    }
    std::fprintf(fp, "#EXTM3U\n");
    for (const auto& p : playlist_.items()) {
        std::fprintf(fp, "%s\n", p.c_str());
    }
    std::fclose(fp);
    showOsd(std::string("Saved  ") + baseName(path.c_str()));
}

void UIController::loadPlaylistM3U(const std::string& path) {
    FILE* fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "rb") != 0 || !fp) {
        showOsd("Open failed"); return;
    }
    std::vector<std::string> paths;
    char line[1024];
    while (std::fgets(line, sizeof(line), fp)) {
        size_t n = std::strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r' || line[n-1] == ' '))
            line[--n] = '\0';
        if (n == 0) continue;
        if (line[0] == '#') continue;  // extinf, comments
        paths.emplace_back(line);
    }
    std::fclose(fp);
    if (paths.empty()) { showOsd("Empty playlist"); return; }
    // Fresh playlist: clear existing then enqueue.
    playlist_.clear();
    enqueuePaths(paths, /*play_first=*/true);
    char buf[80];
    std::snprintf(buf, sizeof(buf), "Loaded  %zu  from  %s",
                  paths.size(), baseName(path.c_str()));
    showOsd(buf);
}
