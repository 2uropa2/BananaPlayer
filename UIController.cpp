#include "UIController.hpp"
#include "License.hpp"
#include "Promo.hpp"
#include "SingleInstance.hpp"
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
#include <functional>
#include <initializer_list>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj_core.h>

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

// Atomic sidecar write: write to `path`.tmp, flush + fclose, then
// MoveFileEx to the real path. On Windows same-volume MoveFileEx is
// atomic, so a crash / power loss during the write leaves the ORIGINAL
// file intact instead of a half-truncated one. Used for every sidecar
// (banana.resume / .bookmarks / .stats / .config / .recent / .session.m3u
// / .hdrlut). Small files (<50 KB) but truncate-then-write was leaving
// users without their last watched position after a crash.
template <class Writer>
bool writeSidecarAtomic(const std::string& path, Writer writer) {
    std::string tmp = path + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    writer(f);
    std::fflush(f);
    std::fclose(f);
#ifdef _WIN32
    if (!MoveFileExA(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(tmp.c_str());
        return false;
    }
#else
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
#endif
    return true;
}

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

// Writable-for-sure sidecar location. When Banana Player is installed
// under C:\Program Files\..., appDir() is admin-only and fopen("wb") on
// banana.config there silently drops the file (no VirtualStore either
// on modern Windows for unmanifested exes) - the symptom is "settings
// don't persist across launches". Route sidecars through
// %APPDATA%\BananaPlayer\ instead. SDL_GetPrefPath creates the directory
// as a side effect and returns a trailing-slash path.
//
// A one-time migration in `migrateSidecarsToDataDir` copies files
// found in appDir() to dataDir() on first read, so users who ran an
// old build from the source tree don't lose their state.
std::string dataDir() {
    static std::string cached;
    if (!cached.empty()) return cached;
    // SDL_GetPrefPath("Banana", "BananaPlayer") -> nested one too deep.
    // Pass empty org so the layout is "%APPDATA%\BananaPlayer\" -
    // matches most well-behaved Windows apps.
    // Falls back to appDir() only if SDL failed (never seen in prod).
    const char* p = SDL_GetPrefPath("", "BananaPlayer");
    if (p && *p) {
        cached = p;
        // SDL_GetPrefPath returns a pointer we don't free (SDL3 owns).
    } else {
        cached = appDir() + "\\";
    }
    return cached;
}

// Migration: for each named sidecar, if it exists in appDir() but NOT
// in dataDir(), copy it over. Preserves state for users who launched
// old builds from the source tree before dataDir was introduced.
// Also copies %APPDATA%\Vela\vela.* -> %APPDATA%\BananaPlayer\banana.*
// so users upgrading from the pre-rename builds keep their settings,
// resume points, bookmarks, recent files, and session playlist.
// Called once from UIController's ctor before any sidecar reads.
static void migrateSidecarsToDataDir() {
    static bool done = false;
    if (done) return;
    done = true;
    // Current-generation sidecar filenames.
    const char* names[] = {
        "banana.config", "banana.resume", "banana.bookmarks",
        "banana.recent", "banana.session.m3u",
        "banana.stats",  "banana.hdrlut"
    };
    std::string dst_base = dataDir();

    // Pass 1: exe-adjacent -> dataDir (source-tree-run users).
    {
        std::string src_base = appDir() + "\\";
        for (const char* nm : names) {
            std::string src = src_base + nm;
            std::string dst = dst_base + nm;
            if (GetFileAttributesA(dst.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
            if (GetFileAttributesA(src.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            CopyFileA(src.c_str(), dst.c_str(), TRUE);
        }
    }

    // Pass 2: legacy %APPDATA%\Vela\vela.* -> %APPDATA%\BananaPlayer\banana.*
    // (Vela-era users upgrading in place).
    {
        const char* appdata = std::getenv("APPDATA");
        if (!appdata) return;
        std::string legacy_dir = std::string(appdata) + "\\Vela\\";
        if (GetFileAttributesA(legacy_dir.c_str()) == INVALID_FILE_ATTRIBUTES) return;
        const char* legacy_names[] = {
            "vela.config", "vela.resume", "vela.bookmarks",
            "vela.recent", "vela.session.m3u",
            "vela.stats",  "vela.hdrlut"
        };
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
            std::string src = legacy_dir + legacy_names[i];
            std::string dst = dst_base + names[i];
            if (GetFileAttributesA(dst.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
            if (GetFileAttributesA(src.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            CopyFileA(src.c_str(), dst.c_str(), TRUE);
        }
        // Legacy window geometry uses the same filename in both eras.
        {
            std::string src = legacy_dir + "vela.window";
            std::string dst = dst_base + "banana.window";
            if (GetFileAttributesA(dst.c_str()) == INVALID_FILE_ATTRIBUTES &&
                GetFileAttributesA(src.c_str()) != INVALID_FILE_ATTRIBUTES) {
                CopyFileA(src.c_str(), dst.c_str(), TRUE);
            }
        }
    }
}

std::string timestampString() {
    // Millisecond-precision so burst-mode shots at short intervals
    // (0.5 s / 1.0 s presets) don't collide inside the same wall-clock
    // second. Two snapshots in the same second used to overwrite each
    // other silently - a 10-shot burst at 0.5 s could end up as 5 files.
    SYSTEMTIME lt;
    GetLocalTime(&lt);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d_%03d",
                  lt.wYear, lt.wMonth, lt.wDay,
                  lt.wHour, lt.wMinute, lt.wSecond, lt.wMilliseconds);
    return buf;
}

// Sidecar files all sit under dataDir() (usually %APPDATA%\BananaPlayer\).
// See dataDir() for the "why not exe-adjacent" rationale.
std::string resumeIndexPath()   { return dataDir() + "banana.resume"; }
std::string bookmarkIndexPath() { return dataDir() + "banana.bookmarks"; }
std::string statsIndexPath()    { return dataDir() + "banana.stats"; }

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
    // One-time migration: pre-1.3.1 builds wrote sidecars next to the
    // exe. That silently failed when the exe lived under Program
    // Files (no admin write access, no UAC virtualization for exes
    // shipped with modern build toolchains). Copy any old sidecars
    // over to dataDir() so existing state survives the switch.
    migrateSidecarsToDataDir();
    loadResumeIndex();
    loadBookmarkIndex();
    loadStatsIndex();
    loadConfig();
    loadRecents();
    loadLastPlaylist();
    // NOTE: window geometry restore intentionally disabled - the earlier
    // implementation left users with a window pinned to (0, 0) that
    // couldn't be dragged (SDL_MaximizeWindow + a bad clamp interaction
    // on the CENTERED sentinel). Re-enable only after both are fixed.
    // applyStoredWindowGeometry();
    // License first, so Promo::start sees the correct suppressed state
    // on its first render (Pro users never flash the promo banner even
    // for a frame). init() also verifies the saved key against the
    // current baked secret; a mismatch means the file was tampered or
    // the key was minted for a different major version.
    License::init(dataDir());
    // Promo slot bootstrap. Fetch runs on a background thread so the
    // splash never waits on network. Cache lives under dataDir().
    Promo::start(dataDir());
}

UIController::~UIController() {
    // Stop the promo fetcher before we start tearing anything down -
    // its background thread must join cleanly before app exit.
    Promo::shutdown();
    snapshotCurrentResume();
    if (resume_dirty_) writeResumeIndex();
    if (bookmark_dirty_) writeBookmarkIndex();
    commitCurrentStats();
    if (stats_dirty_) writeStatsIndex();
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
    freeBitmapSubtitleTexture();
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
    writeSidecarAtomic(resumeIndexPath(), [&](std::FILE* f) {
        size_t n = std::min<size_t>(resume_index_.size(), 200);
        for (size_t i = 0; i < n; ++i) {
            const auto& e = resume_index_[i];
            std::fprintf(f, "%.3f\t%.3f\t%s\n", e.pos, e.duration, e.path.c_str());
        }
    });
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
    writeSidecarAtomic(bookmarkIndexPath(), [&](std::FILE* f) {
        size_t n = std::min<size_t>(bookmark_index_.size(), 200);
        for (size_t i = 0; i < n; ++i) {
            const auto& e = bookmark_index_[i];
            if (e.times.empty()) continue;
            for (size_t j = 0; j < e.times.size(); ++j) {
                std::fprintf(f, "%s%.3f", j ? "," : "", e.times[j]);
            }
            std::fprintf(f, "\t%s\n", e.path.c_str());
        }
    });
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

// ---------- Playback statistics ----------
//
// banana.stats sidecar layout: one line per file.
//   <seconds><TAB><sessions><TAB><path>
// Newest-first (MRU) so trimming to 500 drops the oldest.

void UIController::loadStatsIndex() {
    stats_.clear();
    std::FILE* f = std::fopen(statsIndexPath().c_str(), "rb");
    if (!f) return;
    char buf[8192];
    while (std::fgets(buf, sizeof(buf), f)) {
        std::string line(buf);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line.empty()) continue;
        size_t t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        size_t t2 = line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        double sec  = std::atof(line.substr(0, t1).c_str());
        int    sess = std::atoi(line.substr(t1 + 1, t2 - t1 - 1).c_str());
        std::string p = line.substr(t2 + 1);
        if (p.empty()) continue;
        if (sec < 0.0) sec = 0.0;
        if (sess < 0) sess = 0;
        stats_[p] = PlaybackStats{sec, sess};
    }
    std::fclose(f);
}

void UIController::writeStatsIndex() {
    writeSidecarAtomic(statsIndexPath(), [&](std::FILE* f) {
        // Cap at 500 entries. Snapshot to a vector, sort by session
        // count desc as a rough MRU proxy.
        std::vector<std::pair<std::string, PlaybackStats>> flat(
            stats_.begin(), stats_.end());
        std::sort(flat.begin(), flat.end(),
                  [](const auto& a, const auto& b) {
                      return a.second.sessions > b.second.sessions;
                  });
        size_t n = std::min<size_t>(flat.size(), 500);
        for (size_t i = 0; i < n; ++i) {
            const auto& e = flat[i];
            if (e.second.sessions <= 0 && e.second.seconds <= 0.0) continue;
            std::fprintf(f, "%.3f\t%d\t%s\n",
                         e.second.seconds, e.second.sessions, e.first.c_str());
        }
    });
    stats_dirty_ = false;
}

void UIController::commitCurrentStats() {
    if (!player_ || !player_->isLoaded()) { stats_accum_this_session_ = 0.0; return; }
    const std::string& p = player_->getFilename();
    if (p.empty()) { stats_accum_this_session_ = 0.0; return; }
    if (stats_accum_this_session_ > 0.5) {   // ignore trivial flickers
        stats_[p].seconds += stats_accum_this_session_;
        stats_dirty_ = true;
    }
    stats_accum_this_session_ = 0.0;
}

UIController::PlaybackStats UIController::getStatsFor(const std::string& path) const {
    auto it = stats_.find(path);
    return (it == stats_.end()) ? PlaybackStats{} : it->second;
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
    // Bank the outgoing file's playing time before it's forgotten. Also
    // flush the stats index so a crash on the next load doesn't lose
    // hours of watch history.
    commitCurrentStats();
    if (stats_dirty_) writeStatsIndex();
    // Loading anything cancels an in-flight auto-next countdown - it
    // was set up for the OLD file's natural end and would double-fire.
    cancelAutoNext();
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
    // Video zoom / pan are per-file transients too - a user framing one
    // clip must not carry into the next.
    resetVideoZoomPan();
    // Per-file transient UI state that would otherwise leak into the
    // next clip. In particular subtitles_secondary_ has its own cue
    // times and would render bogus text against a new file's timeline.
    subtitles_secondary_.clear();
    subtitle_secondary_cache_idx_    = 0;
    subtitle_secondary_track_active_ = -1;
    // Embedded subtitle activation is a per-file transient too - a
    // track selected on the previous file's stream indices makes no
    // sense on the new file's, and getEmbeddedSubtitleAt would return
    // stale cues until the decoder is re-opened. VideoPlayer::unload
    // clears its own copy; the UI just doesn't need to preserve a
    // dangling choice.
    // The subtitle-search "last hit" index points into an old cue set;
    // reset so the next Enter starts from the top of the new file.
    subtitle_search_last_hit_ = -1;
    // Abort any in-flight burst so it doesn't keep firing snapshots
    // into the new file. Left-drag pan state is transient input and
    // gets released by the next MOUSE_BUTTON_UP - clear here defensively
    // in case a file switch beat the release event.
    burst_remaining_ = 0;
    burst_next_at_   = 0.0f;
    video_panning_   = false;
    // Chapter announcer resets so the FIRST chapter of the new file
    // gets a "Chapter: X" toast when playback settles on it.
    last_chapter_announced_ = -1;
    // Attempt to auto-load companion .srt / .smi.
    loadCompanionSubs(path);

    if (!player_->loadFile(path)) {
        // Load failed - keep the title/state clean and let the user know
        // which file failed via a clear OSD (longer duration than normal).
        char buf[512]; std::snprintf(buf, sizeof(buf),
                                     "Cannot open  %s", baseName(path.c_str()));
        showOsd(buf, 3.0f);
        SDL_SetWindowTitle(window_, "Banana Player");
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
        // Bump session count for this file, reset accum.
        stats_[path].sessions += 1;
        stats_accum_this_session_ = 0.0;
        stats_dirty_ = true;
        // Update playlist current selection to match (or append)
        bool found = false;
        for (size_t i = 0; i < playlist_.size(); ++i) {
            if (playlist_.items()[i] == path) { playlist_.setCurrent(i); found = true; break; }
        }
        if (!found) {
            playlist_.add(path);
            playlist_.setCurrent(playlist_.size() - 1);
        }

        // Auto-activate an embedded (container) subtitle track when no
        // external sidecar was loaded. Priority:
        //   1. Korean text track (SRT / ASS / mov_text / ...)
        //   2. Korean bitmap track (PGS / DVB / DVD)
        // If the current audio track is NOT Korean, extend to:
        //   3. Any text track
        //   4. Any selectable bitmap track
        // The audio-language guard exists because Korean users watching
        // Korean-audio content typically don't want the container's
        // English subs auto-loaded on top - the previous unconditional
        // "pick any text" fallback made Korean movies feel over-eager.
        if (subtitles_.empty()) {
            auto etracks = player_->getEmbeddedSubtitleTracks();
            auto isTextCodec = [](const std::string& cn) {
                return cn == "subrip" || cn == "ass" || cn == "ssa"
                    || cn == "text"   || cn == "mov_text" || cn == "webvtt";
            };
            auto isKoreanLang = [](const std::string& lg) {
                return lg == "kor" || lg == "ko";
            };
            int pick = -1;
            auto pickIf = [&](bool wantText, bool wantKorean) {
                for (size_t i = 0; i < etracks.size(); ++i) {
                    const auto& t = etracks[i];
                    if (!t.selectable) continue;
                    bool isText = isTextCodec(t.codec_name);
                    if (wantText != isText) continue;
                    if (wantKorean && !isKoreanLang(t.language)) continue;
                    pick = (int)i;
                    return true;
                }
                return false;
            };
            std::string audio_lang = player_->getCurrentAudioLanguage();
            bool audio_is_korean   = isKoreanLang(audio_lang);
            if (!pickIf(true,  true))                       // Korean text
                if (!pickIf(false, true))                   // Korean bitmap
                    if (!audio_is_korean) {
                        if (!pickIf(true,  false))          // any text
                            pickIf(false, false);           // any bitmap
                    }
            if (pick >= 0) {
                if (player_->setActiveEmbeddedSubtitle(pick)) {
                    // Subtle OSD so the user knows subs were auto-picked.
                    // Otherwise a first-time MKV viewer sees text appear
                    // out of nowhere with no explanation.
                    const auto& t = etracks[pick];
                    char m[192];
                    if (!t.language.empty()) {
                        std::snprintf(m, sizeof(m),
                                      "Embedded subs: %s (%s)",
                                      t.codec_name.c_str(),
                                      t.language.c_str());
                    } else {
                        std::snprintf(m, sizeof(m),
                                      "Embedded subs: %s",
                                      t.codec_name.c_str());
                    }
                    showOsd(m, 1.6f);
                }
            }
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
        std::snprintf(title, sizeof(title), "%s  -  Banana Player",
                      baseName(path.c_str()));
        SDL_SetWindowTitle(window_, title);

        // Announce whether hardware decode is engaged so the user knows.
        if (player_->isHardwareDecoding()) {
            char buf[64]; std::snprintf(buf, sizeof(buf), "HW decode  %s",
                                        player_->getHardwareDecoderName());
            showOsd(buf, 1.4f);
        }
        // Announce HDR - show whether tone mapping is engaged or the
        // user has turned it off for native passthrough.
        {
            auto kind = player_->getHdrKind();
            const bool on = player_->isHdrToneMapEnabled();
            if (kind == VideoPlayer::HdrKind::PQ) {
                showOsd(on ? "HDR tone map  PQ -> SDR"
                           : "HDR (PQ)  native passthrough", 1.8f);
            } else if (kind == VideoPlayer::HdrKind::HLG) {
                showOsd(on ? "HDR tone map  HLG -> SDR"
                           : "HDR (HLG)  native passthrough", 1.8f);
            }
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
    // (`BananaPlayer.exe C:\videos`) and drag-drop of a folder do the intuitive
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
    // Single-slot OSD: a new message REPLACES whatever was showing. Prior
    // behavior stacked messages down the right edge, which visually piled
    // up during a burst of nudges (Ctrl+Left / Left + volume wheel, etc.)
    // and each pill occupied a row for its full duration. User prefers a
    // single anchored spot that always reflects the latest state.
    osd_.clear();
    osd_.push_back(OsdMsg{ text, duration, duration });
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
    // Default location: dataDir() (usually %APPDATA%\BananaPlayer\). Was
    // appDir() historically but that's Program Files under a normal
    // install and non-admin writes get dropped. Users who preferred
    // the old exe-adjacent path can point snapshot_dir_ back at it.
    std::string dir  = snapshot_dir_.empty() ? dataDir() : snapshot_dir_;
    // Trim trailing separator so we always join with exactly one '\'.
    while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) {
        dir.pop_back();
    }
    std::string name = std::string("snapshot_") + timestampString() + ".png";
    std::string full = dir + "\\" + name;
    bool ok = false;
    if (snapshot_include_overlays_ && renderer_) {
        // Screen capture route: reads the composited SDL back buffer so
        // subtitles + OSD + any visible HUD ride along. Wait for HUD to
        // fade (3 s idle) before triggering if a "video + subs only"
        // capture is what you're after.
        SDL_Surface* surf = SDL_RenderReadPixels(renderer_, nullptr);
        if (surf) {
            // SDL3 usually returns RGBA/BGRA at 32 bpp with pitch aligned;
            // hand the bytes off to the FFmpeg encoder verbatim - it does
            // its own sws conversion to RGB24 for PNG.
            int avfmt = AV_PIX_FMT_RGBA;
            const SDL_PixelFormatDetails* d = SDL_GetPixelFormatDetails(surf->format);
            if (d) {
                switch (surf->format) {
                    case SDL_PIXELFORMAT_RGBA32:  avfmt = AV_PIX_FMT_RGBA; break;
                    case SDL_PIXELFORMAT_BGRA32:  avfmt = AV_PIX_FMT_BGRA; break;
                    case SDL_PIXELFORMAT_ARGB32:  avfmt = AV_PIX_FMT_ARGB; break;
                    case SDL_PIXELFORMAT_ABGR32:  avfmt = AV_PIX_FMT_ABGR; break;
                    case SDL_PIXELFORMAT_XRGB8888:avfmt = AV_PIX_FMT_0RGB; break;
                    case SDL_PIXELFORMAT_XBGR8888:avfmt = AV_PIX_FMT_0BGR; break;
                    case SDL_PIXELFORMAT_RGBX8888:avfmt = AV_PIX_FMT_RGB0; break;
                    case SDL_PIXELFORMAT_BGRX8888:avfmt = AV_PIX_FMT_BGR0; break;
                    case SDL_PIXELFORMAT_RGB24:   avfmt = AV_PIX_FMT_RGB24; break;
                    case SDL_PIXELFORMAT_BGR24:   avfmt = AV_PIX_FMT_BGR24; break;
                    default:                      avfmt = AV_PIX_FMT_RGBA; break;
                }
            }
            ok = VideoPlayer::snapshotFromBytes(full, surf->w, surf->h,
                                                avfmt, surf->pitch,
                                                (const uint8_t*)surf->pixels);
            SDL_DestroySurface(surf);
        }
    } else {
        ok = player_->snapshot(full);
    }
    if (ok) {
        snapshot_flash_ = 1.0f;
        // Suppress the per-file "Saved <path>" OSD while a burst is in
        // progress - the burst tick shows a "Burst N/M" progress toast
        // instead so the OSD stack doesn't balloon.
        if (burst_remaining_ == 0) {
            showOsd(std::string("Saved  ") + name);
        }
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
    subtitles_secondary_.clear();
    subtitle_secondary_cache_idx_ = 0;
    subtitle_secondary_track_active_ = -1;
    loop_a_ = -1.0;
    loop_b_ = -1.0;
    chapters_.clear();
    bookmarks_current_.clear();
    // Not playing a YouTube-resolved stream anymore - drop the source URL
    // so the top-overlay quality pill disappears.
    current_yt_source_url_.clear();
    if (thumb_provider_) thumb_provider_->close();
    SDL_SetWindowTitle(window_, "Banana Player");
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

bool UIController::loadSubtitleSidecarSecondary(const std::string& path) {
    // Same parser plumbing as loadSubtitleSidecar - it just lands in
    // subtitles_secondary_ instead. Used by the dual-subtitle workflow
    // (learn-a-language: primary in native tongue, secondary in target).
    if (path.empty()) return false;
    std::string utf8 = decodeSubtitleFileToUtf8(path);
    std::string ext = path.size() >= 4 ? path.substr(path.size() - 4) : "";
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
    std::vector<SubtitleCue> cues;
    if (ext == ".srt") cues = parseSRT(utf8);
    else               cues = parseSMI(utf8);
    if (cues.empty()) return false;
    subtitles_secondary_ = std::move(cues);
    subtitle_secondary_cache_idx_ = 0;
    return true;
}

const SubtitleCue* UIController::currentSubtitleSecondary(double t) {
    // Mirror of currentSubtitle() operating on subtitles_secondary_.
    // Keeps its own cache index so a mid-scan of one track doesn't
    // invalidate the linear-skip optimization on the other.
    if (subtitles_secondary_.empty()) return nullptr;
    t -= subtitle_offset_;
    auto in = [](const SubtitleCue& c, double t) {
        return t >= c.start && t <= c.end;
    };
    if (subtitle_secondary_cache_idx_ >= 0
        && subtitle_secondary_cache_idx_ < (int)subtitles_secondary_.size()
        && in(subtitles_secondary_[subtitle_secondary_cache_idx_], t)) {
        return &subtitles_secondary_[subtitle_secondary_cache_idx_];
    }
    int idx = subtitle_secondary_cache_idx_;
    if (idx < 0) idx = 0;
    for (int i = idx; i < (int)subtitles_secondary_.size(); ++i) {
        if (in(subtitles_secondary_[i], t)) {
            subtitle_secondary_cache_idx_ = i;
            return &subtitles_secondary_[i];
        }
        if (subtitles_secondary_[i].start > t + 0.1) break;
    }
    // Fallback: full scan (backward jump / seek).
    for (int i = 0; i < (int)subtitles_secondary_.size(); ++i) {
        if (in(subtitles_secondary_[i], t)) {
            subtitle_secondary_cache_idx_ = i;
            return &subtitles_secondary_[i];
        }
    }
    return nullptr;
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

void UIController::resetVideoZoomPan() {
    video_zoom_  = 1.0f;
    video_pan_x_ = 0.0f;
    video_pan_y_ = 0.0f;
    video_panning_ = false;
}

// Local helper: pick the currently-visible subtitle text (primary preferred,
// fall back to secondary). Returns an empty string when nothing is on screen.
static std::string pickVisibleSubtitleText(UIController* ui,
                                           VideoPlayer* player,
                                           const std::vector<SubtitleCue>* pri,
                                           const std::vector<SubtitleCue>* sec) {
    (void)ui;
    if (!player || !player->isLoaded()) return {};
    double t = player->getCurrentTime();
    auto scan = [&](const std::vector<SubtitleCue>* cues) -> const SubtitleCue* {
        if (!cues || cues->empty()) return nullptr;
        for (const auto& c : *cues) {
            if (t >= c.start && t < c.end) return &c;
        }
        return nullptr;
    };
    if (const SubtitleCue* c = scan(pri)) return c->text;
    if (const SubtitleCue* c = scan(sec)) return c->text;
    return {};
}

// URL-encode a UTF-8 string for use in a query parameter. Only reserved
// characters get percent-encoded; everything else passes through.
static std::string urlEncode(const std::string& s) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        bool safe = (c >= '0' && c <= '9')
                 || (c >= 'A' && c <= 'Z')
                 || (c >= 'a' && c <= 'z')
                 || c == '-' || c == '_' || c == '.' || c == '~';
        if (safe) out.push_back((char)c);
        else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0xF]);
            out.push_back(kHex[c & 0xF]);
        }
    }
    return out;
}

void UIController::copyCurrentSubtitleToClipboard() {
    std::string text = pickVisibleSubtitleText(this, player_,
                            &subtitles_, &subtitles_secondary_);
    if (text.empty()) { showOsd("No subtitle on screen", 1.4f); return; }
    // Convert UTF-8 to UTF-16 for CF_UNICODETEXT.
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (wlen <= 0) { showOsd("Clipboard copy failed", 1.6f); return; }
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)wlen * sizeof(wchar_t));
    if (!h) { showOsd("Clipboard copy failed", 1.6f); return; }
    wchar_t* p = (wchar_t*)GlobalLock(h);
    if (!p) { GlobalFree(h); showOsd("Clipboard copy failed", 1.6f); return; }
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, p, wlen);
    GlobalUnlock(h);
    if (!OpenClipboard(nullptr)) { GlobalFree(h); showOsd("Clipboard busy", 1.6f); return; }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, h)) {
        GlobalFree(h);
    }
    // On success, clipboard owns h - do NOT free.
    CloseClipboard();
    showOsd("Subtitle copied to clipboard", 1.4f);
}

void UIController::lookupCurrentSubtitleInBrowser() {
    std::string text = pickVisibleSubtitleText(this, player_,
                            &subtitles_, &subtitles_secondary_);
    if (text.empty()) { showOsd("No subtitle on screen", 1.4f); return; }
    // Collapse embedded newlines / carriage returns to spaces so the
    // URL doesn't break on multi-line cues.
    for (char& c : text) if (c == '\n' || c == '\r') c = ' ';
    std::string url = "https://www.google.com/search?q=" + urlEncode(text);
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    showOsd("Opening browser lookup", 1.4f);
}

// Compute the OpenSubtitles 64-bit movie hash for a local file. Returns
// 0 on any I/O failure. The algorithm: hash = file_size + sum of the
// first 64 KB (as uint64le) + sum of the last 64 KB (as uint64le).
// Files smaller than 128 KB return 0 (the algorithm requires 64 KB at
// each end).
static uint64_t openSubtitlesHash(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return 0;
    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return 0; }
    long size_l = std::ftell(f);
    if (size_l < 131072) { std::fclose(f); return 0; }
    uint64_t size = (uint64_t)size_l;
    uint64_t h = size;
    auto sumChunk = [&](long offset) -> bool {
        if (std::fseek(f, offset, SEEK_SET) != 0) return false;
        uint64_t buf[8192];   // 64 KB
        if (std::fread(buf, 1, sizeof(buf), f) != sizeof(buf)) return false;
        for (int i = 0; i < 8192; ++i) h += buf[i];   // LE machine assumed
        return true;
    };
    if (!sumChunk(0) || !sumChunk(size_l - 65536)) {
        std::fclose(f); return 0;
    }
    std::fclose(f);
    return h;
}

void UIController::searchSubtitlesOnline() {
    if (!player_->isLoaded()) { showOsd("No video loaded"); return; }
    const std::string& src = player_->getFilename();
    if (src.empty()) { showOsd("No source path"); return; }
    // For remote / stream sources we can't compute a hash - just fall
    // back to name-only search.
    std::string base = baseName(src.c_str());
    size_t dot = base.find_last_of('.');
    std::string stem = (dot != std::string::npos) ? base.substr(0, dot) : base;

    uint64_t hash = openSubtitlesHash(src);
    char hash_hex[32];
    hash_hex[0] = '\0';
    if (hash != 0) {
        std::snprintf(hash_hex, sizeof(hash_hex), "%016llx",
                      (unsigned long long)hash);
    }

    // The public search page accepts both the query text and the
    // moviehash parameter. Users pick their language + download the
    // .srt; dragging it onto Banana Player loads it via the existing drop
    // handler. No API key, no rate limits, no bundled HTTP client.
    std::string url = "https://www.opensubtitles.com/en/search-all?q="
                    + urlEncode(stem);
    if (hash_hex[0]) {
        url += "&MovieHash=";
        url += hash_hex;
    }
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (hash_hex[0]) {
        showOsd("Opened OpenSubtitles (hash matched)", 2.0f);
    } else {
        showOsd("Opened OpenSubtitles (name search)", 2.0f);
    }
}

void UIController::exportABSegmentAsGif() {
    if (!player_->isLoaded()) { showOsd("No video loaded"); return; }
    if (loop_a_ < 0.0 || loop_b_ < 0.0) {
        showOsd("Set A and B first (press A twice)", 2.0f); return;
    }
    // GIF encoding via palettegen holds every scaled frame in memory
    // for palette-full stats. Cap at 20 s so a mis-set A-B doesn't
    // pin gigabytes of RAM. Users who want longer GIFs can chain a few.
    double dur = loop_b_ - loop_a_;
    if (dur > 20.0) {
        showOsd("Cap GIF export at 20s (trim A-B)", 2.4f); return;
    }
    if (dur < 0.10) {
        showOsd("A-B segment too short", 1.6f); return;
    }
    const std::string& src = player_->getFilename();
    if (src.empty()) { showOsd("No source path"); return; }
    std::string base = baseName(src.c_str());
    size_t dot = base.find_last_of('.');
    std::string stem = (dot != std::string::npos) ? base.substr(0, dot) : base;
    char suggest[MAX_PATH];
    std::snprintf(suggest, sizeof(suggest), "%s_clip_%d-%d.gif",
                  stem.c_str(),
                  (int)std::lround(loop_a_),
                  (int)std::lround(loop_b_));

    OPENFILENAMEA ofn = {};
    char out_path[MAX_PATH];
    std::strncpy(out_path, suggest, sizeof(out_path) - 1);
    out_path[sizeof(out_path) - 1] = '\0';
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile   = out_path;
    ofn.nMaxFile    = sizeof(out_path);
    ofn.lpstrFilter = "Animated GIF (*.gif)\0*.gif\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrDefExt = "gif";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT
              | OFN_NOCHANGEDIR | OFN_EXPLORER;
    ofn.lpstrTitle = "Save A-B segment as GIF...";
    if (!GetSaveFileNameA(&ofn)) return;

    showOsd("Exporting GIF...", 3.0f);
    // Palettegen + paletteuse is slow enough that a UI freeze is
    // noticeable; the OSD above at least warns the user. 15 fps and
    // 480 px width are the standard reddit-clip defaults - readable,
    // reasonable file size, doesn't scare the browser upload dialog.
    bool ok = VideoPlayer::exportSegmentAsGif(src, out_path,
                                              loop_a_, loop_b_,
                                              /*fps=*/15, /*width=*/480);
    if (ok) {
        char msg[MAX_PATH + 32];
        std::snprintf(msg, sizeof(msg), "GIF saved  %s", baseName(out_path));
        showOsd(msg, 2.6f);
    } else {
        showOsd("GIF export failed", 2.4f);
    }
}

void UIController::exportABSegmentToFile() {
    if (!player_->isLoaded()) { showOsd("No video loaded"); return; }
    if (loop_a_ < 0.0 || loop_b_ < 0.0) {
        showOsd("Set A and B first (press A twice)", 2.0f);
        return;
    }
    if (loop_b_ - loop_a_ < 0.10) {
        showOsd("A-B segment too short", 1.6f);
        return;
    }

    // Suggest a filename based on the source basename + timestamps.
    const std::string& src = player_->getFilename();
    if (src.empty()) { showOsd("No source path"); return; }
    std::string base = baseName(src.c_str());
    // Strip extension for the default name suggestion.
    size_t dot = base.find_last_of('.');
    std::string stem = (dot != std::string::npos) ? base.substr(0, dot) : base;
    // Preserve source extension so the muxer keeps the same container.
    std::string ext = (dot != std::string::npos) ? base.substr(dot) : ".mp4";
    char suggest[MAX_PATH];
    std::snprintf(suggest, sizeof(suggest), "%s_clip_%d-%d%s",
                  stem.c_str(),
                  (int)std::lround(loop_a_),
                  (int)std::lround(loop_b_),
                  ext.c_str());

    OPENFILENAMEA ofn = {};
    char out_path[MAX_PATH];
    std::strncpy(out_path, suggest, sizeof(out_path) - 1);
    out_path[sizeof(out_path) - 1] = '\0';
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile   = out_path;
    ofn.nMaxFile    = sizeof(out_path);
    ofn.lpstrFilter =
        "Video (*.mp4;*.mkv;*.mov)\0*.mp4;*.mkv;*.mov\0"
        "All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT
              | OFN_NOCHANGEDIR | OFN_EXPLORER;
    ofn.lpstrTitle = "Save A-B segment as...";
    if (!GetSaveFileNameA(&ofn)) return;   // user cancelled

    showOsd("Exporting A-B segment...", 3.0f);
    // Synchronous - typical stream copy is fast (100 MB/s+ on SSD) so
    // this stalls the UI for a second or two on ordinary clips. Users
    // see the "Exporting..." OSD until the OK/fail toast replaces it.
    bool ok = VideoPlayer::exportSegment(src, out_path, loop_a_, loop_b_);
    if (ok) {
        char msg[MAX_PATH + 32];
        std::snprintf(msg, sizeof(msg), "Saved  %s", baseName(out_path));
        showOsd(msg, 2.6f);
    } else {
        showOsd("Export failed", 2.4f);
    }
}

void UIController::openSnapshotFolderDialog() {
    // Classic SHBrowseForFolder - modern IFileOpenDialog is nicer but
    // needs full COM plumbing. This dialog is used once in a blue moon,
    // so keep it lean.
    BROWSEINFOA bi{};
    bi.hwndOwner = nullptr;
    bi.lpszTitle = "Choose the folder Banana Player snapshots should land in";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (!pidl) return;   // user cancelled
    char picked[MAX_PATH]{};
    if (SHGetPathFromIDListA(pidl, picked)) {
        DWORD attrs = GetFileAttributesA(picked);
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            snapshot_dir_ = picked;
            char msg[MAX_PATH + 32];
            std::snprintf(msg, sizeof(msg), "Snapshots -> %s", picked);
            showOsd(msg, 2.4f);
            markConfigDirty();
        } else {
            showOsd("Selected path is not a directory", 2.0f);
        }
    }
    CoTaskMemFree(pidl);
}

void UIController::openSubtitleSearch() {
    if (subtitles_.empty() && subtitles_secondary_.empty()) {
        showOsd("No subtitles to search", 1.4f);
        return;
    }
    show_subtitle_search_ = true;
    subtitle_search_focus_next_ = true;
}

void UIController::runSubtitleSearchStep(int direction) {
    // Case-insensitive substring search across subtitles_ AND
    // subtitles_secondary_, in cue-start order. direction: +1 forward,
    // -1 backward. Wraps at ends. Seeks to the found cue's start.
    if (subtitle_search_query_[0] == '\0') return;
    // Merge cues from both tracks tagged with (kind, index) so we can
    // step forward/backward through combined timeline order.
    struct H { double start; std::string text; };
    std::vector<H> all;
    all.reserve(subtitles_.size() + subtitles_secondary_.size());
    for (const auto& c : subtitles_)           all.push_back({c.start, c.text});
    for (const auto& c : subtitles_secondary_) all.push_back({c.start, c.text});
    if (all.empty()) return;
    std::sort(all.begin(), all.end(),
              [](const H& a, const H& b){ return a.start < b.start; });

    std::string q(subtitle_search_query_);
    for (auto& c : q) c = (char)std::tolower((unsigned char)c);

    auto lc = [](std::string s) {
        for (auto& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };

    int n = (int)all.size();
    int start = subtitle_search_last_hit_;
    if (start < 0 || start >= n) start = -1;
    for (int step = 1; step <= n; ++step) {
        int i = (start + step * direction + n) % n;
        if (lc(all[i].text).find(q) != std::string::npos) {
            subtitle_search_last_hit_ = i;
            player_->seek(all[i].start);
            char buf[64]; std::snprintf(buf, sizeof(buf),
                                        "Sub match at %s",
                                        formatTime(all[i].start).c_str());
            showOsd(buf, 1.4f);
            return;
        }
    }
    showOsd("No match", 1.2f);
}

void UIController::drawSubtitleSearchPanel(int W, int H) {
    if (!show_subtitle_search_) return;
    // Small centered floating panel: text input + Prev/Next buttons +
    // hint line. Sits above the video, below the top overlay so it
    // doesn't hide the seek bar.
    const float panel_w = 380.0f;
    const float panel_h = 90.0f;
    ImGui::SetNextWindowPos(ImVec2((W - panel_w) * 0.5f,
                                    H * 0.20f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panel_w, panel_h), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
    if (ImGui::Begin("##subsearch", nullptr,
                     ImGuiWindowFlags_NoTitleBar
                     | ImGuiWindowFlags_NoResize
                     | ImGuiWindowFlags_NoCollapse
                     | ImGuiWindowFlags_NoScrollbar)) {
        if (subtitle_search_focus_next_) {
            ImGui::SetKeyboardFocusHere();
            subtitle_search_focus_next_ = false;
        }
        ImGui::SetNextItemWidth(-1.0f);
        bool submitted = ImGui::InputTextWithHint(
                            "##sq", "Search subtitle text (Enter = next)",
                            subtitle_search_query_,
                            sizeof(subtitle_search_query_),
                            ImGuiInputTextFlags_EnterReturnsTrue);
        if (submitted) runSubtitleSearchStep(+1);
        if (ImGui::Button("< Prev", ImVec2(80, 26))) runSubtitleSearchStep(-1);
        ImGui::SameLine();
        if (ImGui::Button("Next >", ImVec2(80, 26))) runSubtitleSearchStep(+1);
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(80, 26))) show_subtitle_search_ = false;
        ImGui::TextDisabled("Ctrl+F to open  .  Esc to close");
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void UIController::triggerBurstSnapshot() {
    if (!player_->isLoaded()) { showOsd("No video loaded"); return; }
    if (burst_remaining_ > 0) {
        // A second press mid-burst cancels the remaining shots. Nice
        // escape hatch if the user realizes they mistimed the trigger.
        burst_remaining_ = 0;
        burst_next_at_   = 0.0f;
        showOsd("Burst cancelled", 1.2f);
        return;
    }
    burst_remaining_ = burst_count_;
    burst_next_at_   = 0.0f;   // first shot on the next tick
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Burst %d x %.1fs",
                  burst_count_, burst_interval_);
    showOsd(buf, 1.6f);
}

void UIController::cycleSubtitleTrack() {
    int n = (int)subtitle_tracks_.size();
    if (n == 0) { showOsd("No subtitle files found", 1.4f); return; }
    if (n == 1) { showOsd("Only one subtitle track", 1.4f); return; }
    int next = (subtitle_track_active_ + 1) % n;
    if (loadSubtitleSidecar(subtitle_tracks_[next].path)) {
        subtitle_track_active_ = next;
        subtitles_enabled_ = true;
        char buf[192];
        std::snprintf(buf, sizeof(buf), "Subs: %s",
                      subtitle_tracks_[next].label.c_str());
        showOsd(buf, 1.6f);
    } else {
        showOsd("Subtitle load failed", 2.0f);
    }
}

void UIController::cycleAudioTrack() {
    if (!player_ || !player_->isLoaded()) return;
    int n = player_->getAudioTrackCount();
    if (n <= 1) {
        showOsd(n == 0 ? "No audio track" : "Only one audio track", 1.4f);
        return;
    }
    int cur  = player_->getCurrentAudioTrackIndex();
    int next = (cur < 0) ? 0 : ((cur + 1) % n);
    if (!player_->setAudioTrack(next)) {
        showOsd("Failed to switch audio track", 1.6f);
        return;
    }
    std::string name = player_->getAudioTrackName(next);
    if (name.empty()) {
        char buf[64]; std::snprintf(buf, sizeof(buf), "Audio track %d / %d", next + 1, n);
        showOsd(buf, 1.8f);
    } else {
        // Trim any trailing extra info to keep the OSD short.
        showOsd(name.c_str(), 1.8f);
    }
}

void UIController::togglePipMode() {
    if (!window_) return;
    if (fullscreen_
        || (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0) {
        showOsd("Not while fullscreen");
        return;
    }
    if (pip_active_) {
        // Exit PiP: restore geometry, always-on-top state, and the
        // 640x400 minimum size gate. Un-maximized state restored only
        // if we found the window maximized on entry (kept the mode).
        SDL_SetWindowMinimumSize(window_, 640, 400);
        SDL_SetWindowSize(window_, pip_saved_w_, pip_saved_h_);
        SDL_SetWindowPosition(window_, pip_saved_x_, pip_saved_y_);
        SDL_SetWindowAlwaysOnTop(window_, pip_saved_always_on_top_);
        always_on_top_ = pip_saved_always_on_top_;
        if (pip_was_maximized_) SDL_MaximizeWindow(window_);
        pip_active_ = false;
        showOsd("Picture in picture off");
        return;
    }
    // Enter PiP: remember current window state, then shrink + pin.
    SDL_WindowFlags flags = SDL_GetWindowFlags(window_);
    pip_was_maximized_ = (flags & SDL_WINDOW_MAXIMIZED) != 0;
    if (pip_was_maximized_) SDL_RestoreWindow(window_);
    SDL_GetWindowPosition(window_, &pip_saved_x_, &pip_saved_y_);
    SDL_GetWindowSize(window_, &pip_saved_w_, &pip_saved_h_);
    pip_saved_always_on_top_ = always_on_top_;

    // Compact size honoring the current video aspect (post-rotation).
    // Falls back to 480x270 if no video is loaded.
    int target_w = 480, target_h = 270;
    if (player_ && player_->isLoaded()) {
        int vw = player_->getVideoWidth();
        int vh = player_->getVideoHeight();
        if (vw > 0 && vh > 0) {
            int total = ((player_->getIntrinsicRotation()
                          + ((int)rotation_ * 90)) % 360 + 360) % 360;
            bool rot90 = (total == 90 || total == 270);
            int ew = rot90 ? vh : vw;
            int eh = rot90 ? vw : vh;
            const int kPipTargetLong = 480;
            if (ew >= eh) {
                target_w = kPipTargetLong;
                target_h = std::max(180, (int)((double)eh * kPipTargetLong / ew));
            } else {
                target_h = kPipTargetLong;
                target_w = std::max(180, (int)((double)ew * kPipTargetLong / eh));
            }
        }
    }
    // Lower the minimum-size gate so 480px width actually applies -
    // the ctor-installed 640x400 clamp would otherwise reject the resize.
    SDL_SetWindowMinimumSize(window_, 240, 160);
    SDL_SetWindowSize(window_, target_w, target_h);

    // Place in the bottom-right of the display's usable bounds with a
    // small margin so it doesn't sit under the taskbar.
    SDL_DisplayID disp = SDL_GetDisplayForWindow(window_);
    if (disp == 0) disp = SDL_GetPrimaryDisplay();
    SDL_Rect bounds{};
    if (SDL_GetDisplayUsableBounds(disp, &bounds)) {
        const int margin = 24;
        SDL_SetWindowPosition(window_,
            bounds.x + bounds.w - target_w - margin,
            bounds.y + bounds.h - target_h - margin);
    }

    SDL_SetWindowAlwaysOnTop(window_, true);
    always_on_top_ = true;
    pip_active_ = true;
    showOsd("Picture in picture", 1.6f);
}

void UIController::toggleHdrToneMap() {
    hdr_tonemap_enabled_ = !hdr_tonemap_enabled_;
    if (player_) {
        player_->setHdrToneMapEnabled(hdr_tonemap_enabled_);
        // Flush already-decoded frames so the toggle is visible right
        // away instead of after the ~8-frame video queue drains.
        if (player_->isLoaded()
            && player_->getHdrKind() != VideoPlayer::HdrKind::None) {
            player_->seekRelative(0.0);
        }
    }
    if (player_ && player_->getHdrKind() != VideoPlayer::HdrKind::None) {
        showOsd(hdr_tonemap_enabled_
                    ? "HDR tone map on"
                    : "HDR tone map off (native passthrough)", 1.6f);
    } else {
        showOsd(hdr_tonemap_enabled_
                    ? "HDR tone map on (no HDR file loaded)"
                    : "HDR tone map off (no HDR file loaded)", 1.6f);
    }
    markConfigDirty();
}

void UIController::drawAutoNextOverlay(int W, int H) {
    if (!auto_next_active_) return;
    // Sit a compact panel in the top-right corner (out of the way of
    // the video's central content and the bottom overlay). Sized so
    // the two buttons fit at min-window (640 wide).
    const float pad_x = 12.0f;
    const float pad_y = 10.0f;
    const float panel_w = 320.0f;
    const float panel_h = 100.0f;
    float x = (float)W - panel_w - 16.0f;
    float y = 16.0f;
    if (x < 8.0f) x = 8.0f;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(x + 2, y + 5),
                      ImVec2(x + panel_w + 2, y + panel_h + 5),
                      IM_COL32(0, 0, 0, 120), 8.0f);
    dl->AddRectFilled(ImVec2(x, y),
                      ImVec2(x + panel_w, y + panel_h),
                      IM_COL32(18, 24, 34, 235), 8.0f);
    dl->AddRect(ImVec2(x, y),
                ImVec2(x + panel_w, y + panel_h),
                IM_COL32(96, 176, 246, 180), 8.0f, 0, 1.5f);

    ImFont* font = ImGui::GetFont();
    int secs_left = (int)std::ceil(auto_next_secs_);
    if (secs_left < 1) secs_left = 1;
    char head[64];
    std::snprintf(head, sizeof(head), "Next in %d ...", secs_left);
    dl->AddText(font, 16.0f, ImVec2(x + pad_x, y + pad_y),
                IM_COL32(220, 230, 250, 255), head);
    dl->AddText(font, 12.0f, ImVec2(x + pad_x, y + pad_y + 22.0f),
                IM_COL32(170, 182, 205, 220),
                "Playlist finished this item.");

    // Buttons row at bottom.
    float bw = 88.0f, bh = 26.0f;
    float row_y = y + panel_h - pad_y - bh + 4.0f;
    ImGui::SetCursorScreenPos(ImVec2(x + pad_x, row_y));
    ImGui::PushID("##autonext_skip");
    if (Style::TextButton("Play now", ImVec2(bw, bh), true)) {
        auto_next_active_ = false;
        auto_next_secs_   = 0.0f;
        handleNaturalEndImmediate();
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(x + pad_x + bw + 8.0f, row_y));
    ImGui::PushID("##autonext_cancel");
    if (Style::TextButton("Cancel", ImVec2(bw, bh), false)) {
        cancelAutoNext();
    }
    ImGui::PopID();
}

void UIController::drawFirstRunHint(int W, int H) {
    if (!first_run_hint_pending_) return;
    // Suppress once a file loads - the user is on their way, no need
    // for the banner. `saveConfig` on quit still writes first_run_done=1
    // so the banner never comes back on the next launch.
    if (player_ && player_->isLoaded()) return;

    // Sit a modest floating panel at the bottom center of the splash,
    // above the version footer, wide enough for the four tip lines but
    // clamped to the window so 640x400 layouts don't clip.
    const float pad = 14.0f;
    float panel_w = std::min((float)W * 0.90f, 560.0f);
    float panel_h = 152.0f;
    float x = ((float)W - panel_w) * 0.5f;
    float y = (float)H * 0.72f;
    // Never overlap the splash version footer at the very bottom.
    if (y + panel_h > (float)H - 40.0f) y = (float)H - 40.0f - panel_h;
    if (y < 20.0f) y = 20.0f;

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    // Drop shadow.
    dl->AddRectFilled(ImVec2(x + 2, y + 6),
                      ImVec2(x + panel_w + 2, y + panel_h + 6),
                      IM_COL32(0, 0, 0, 120), 10.0f);
    // Panel body - slightly warm-dark so it reads as "helpful" not
    // "error"; banana-yellow hairline border keeps it on-brand.
    dl->AddRectFilled(ImVec2(x, y),
                      ImVec2(x + panel_w, y + panel_h),
                      IM_COL32(26, 22, 16, 245), 10.0f);
    dl->AddRect(ImVec2(x, y),
                ImVec2(x + panel_w, y + panel_h),
                IM_COL32(255, 201, 58, 200), 10.0f, 0, 1.5f);

    ImFont* font = ImGui::GetFont();
    float hy = y + pad;
    dl->AddText(font, 17.0f, ImVec2(x + pad, hy),
                IM_COL32(255, 222, 122, 255), "Welcome to Banana Player");
    hy += 22.0f;
    const char* tips[] = {
        "Drop any video / image onto the window to play it",
        "Right-click on the picture for the full menu",
        "Click the gear icon (or Ctrl+,) to open Settings",
        "F1 or ?  to see every keyboard shortcut",
    };
    for (const char* t : tips) {
        dl->AddText(font, 13.5f, ImVec2(x + pad, hy),
                    IM_COL32(220, 230, 250, 240), t);
        hy += 18.0f;
    }

    // "Got it" dismiss button at bottom-right of the panel.
    float bw = 90.0f, bh = 26.0f;
    float bx = x + panel_w - pad - bw;
    float by = y + panel_h - pad - bh + 4.0f;
    ImGui::SetCursorScreenPos(ImVec2(bx, by));
    ImGui::PushID("##firstrun_dismiss");
    if (Style::TextButton("Got it", ImVec2(bw, bh), true)) {
        first_run_hint_pending_ = false;
        markConfigDirty();
    }
    ImGui::PopID();
}

void UIController::drawShortcutsPanel(int W, int H) {
    if (!show_shortcuts_) return;

    // Background draw list so the ImGui window's text (drawn to the window's
    // own list) lands ON TOP of these rects. If we used the foreground draw
    // list here - the pattern About uses - our text would be overpainted by
    // the panel fill and the modal would look empty.
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
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
            { "F  /  Enter  /  F11  /  Alt+Enter", "Fullscreen" },
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
            { "Ctrl + Home / End","Also jump to start / near end" },
            { "Ctrl + G",        "Go to timestamp (H:MM:SS)" },
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
            { "Ctrl + A",        "Cycle audio track" },
            { "Media keys",      "Play/Pause  Stop  Next  Prev  (window focused)" },
            { "Up  /  Down",     "Volume +/- 5 %  (up to 200 %)" },
            { "+  /  -",         "Same as Up / Down" },
            { "Mouse wheel",     "Volume (over video)" },
            { "M",               "Mute" },
            { "Shift + N",       "Normalizer toggle (loudness)" },
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
            { "F4 / Backspace",  "Close current file, back to splash" },
            { "F5",              "Reload current file" },
            { "Ctrl + Q",        "Quit" },
            { "Ctrl + S",        "Snapshot (PNG next to the app)" },
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
            { "S  /  G  /  J",   "Toggle subs  /  cycle size  /  cycle track" },
            { "Ctrl + F",        "Search subtitle text" },
            { "Ctrl + Shift + G","Burst screenshot (N shots)" },
            { "B",               "Toggle subtitle background box" },
            { "Alt + Up/Down/0", "Raise / lower / reset subtitle position" },
            { "Drop a file",     "Play immediately" },
        }},
        { "View", {
            { "Ctrl + R",        "Rotate 90 deg CW" },
            { "Ctrl + Wheel",    "Video zoom in / out" },
            { "Alt + Drag",      "Pan video (when zoomed)" },
            { "Alt + 0",         "Reset zoom / pan" },
            { "Shift + H",       "HDR tone map on / off (HDR files only)" },
            { "Shift + P",       "Picture-in-picture toggle" },
            { "Ctrl + ,",        "Settings dialog" },
            { "Ctrl + H",        "Playback history" },
            { "I",               "Media info panel" },
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

    const float pw = std::min(520.0f, W - 80.0f);
    // Panel grows tall enough for the license row below the tagline
    // (email + key inputs + Verify + status line) plus the Close button.
    const float ph = 460.0f;
    ImVec2 tl((W - pw) * 0.5f, (H - ph) * 0.5f);
    ImVec2 br(tl.x + pw, tl.y + ph);

    // Soft shadow beneath the panel + subtle inner-glow border for lift.
    // Warm-dark panel with banana-yellow hairline border for brand echo.
    bg->AddRectFilled(ImVec2(tl.x - 3, tl.y - 3), ImVec2(br.x + 3, br.y + 6),
                      IM_COL32(0, 0, 0, 90), 16.0f);
    bg->AddRectFilled(tl, br, IM_COL32(24, 20, 14, 246), 14.0f);
    bg->AddRect(tl, br, IM_COL32(255, 201, 58, 140), 14.0f, 0, 1.0f);

    ImFont* font = ImGui::GetFont();
    float cx = tl.x + pw * 0.5f;

    // Wordmark with a matching yellow accent underline so the About panel
    // echoes the splash treatment (same brand, same rhythm). The Banana
    // Player name is longer than the old VELA so we shrink the display
    // size accordingly - still clearly the hero on a 480 px panel.
    {
        const char* wm = "Banana Player";
        float sz = 34.0f;
        ImVec2 d = font->CalcTextSizeA(sz, FLT_MAX, 0.0f, wm);
        float wm_y = tl.y + 48.0f;
        // Faint accent glow + crisp cream.
        bg->AddText(font, sz, ImVec2(cx - d.x * 0.5f + 0.5f, wm_y + 2.0f),
                    IM_COL32(0, 0, 0, 80), wm);
        bg->AddText(font, sz, ImVec2(cx - d.x * 0.5f, wm_y),
                    IM_COL32(255, 201, 58, 40), wm);
        bg->AddText(font, sz, ImVec2(cx - d.x * 0.5f, wm_y),
                    IM_COL32(248, 244, 232, 255), wm);
        // Yellow accent underline.
        float bar_w = d.x * 0.60f;
        float bar_y = wm_y + sz + 4.0f;
        bg->AddRectFilledMultiColor(
            ImVec2(cx - bar_w * 0.5f, bar_y),
            ImVec2(cx + bar_w * 0.5f, bar_y + 2.0f),
            IM_COL32(227, 154,  28,   0),
            IM_COL32(255, 222, 122, 255),
            IM_COL32(255, 222, 122, 255),
            IM_COL32(227, 154,  28,   0));
    }
    // Korean sub-mark: UTF-8 hex escapes for the "banana player" name in
    // hangul (see docs/BRAND_BANANA.md for the Latinized reading). Source
    // stays pure ASCII so MSVC doesn't emit C4819 on the Korean-codepage
    // host; runtime decode happens through the Korean glyph range merged
    // into the main font atlas in Style::LoadFonts.
    {
        const char* wmk = "\xEB\xB0\x94\xEB\x82\x98\xEB\x82\x98 "
                          "\xED\x94\x8C\xEB\xA0\x88\xEC\x9D\xB4\xEC\x96\xB4";
        float sz = 15.0f;
        ImVec2 d = font->CalcTextSizeA(sz, FLT_MAX, 0.0f, wmk);
        bg->AddText(font, sz, ImVec2(cx - d.x * 0.5f, tl.y + 96.0f),
                    IM_COL32(210, 195, 160, 220), wmk);
    }
    // Version
    {
        char v[48];
        std::snprintf(v, sizeof(v), "Version %s", BP_VERSION);
        float sz = 14.0f;
        ImVec2 d = font->CalcTextSizeA(sz, FLT_MAX, 0.0f, v);
        bg->AddText(font, sz, ImVec2(cx - d.x * 0.5f, tl.y + 128.0f),
                    IM_COL32(220, 200, 150, 220), v);
    }
    // Stack
    {
        const char* s = "SDL3  .  Dear ImGui  .  FFmpeg";
        float sz = 14.0f;
        ImVec2 d = font->CalcTextSizeA(sz, FLT_MAX, 0.0f, s);
        bg->AddText(font, sz, ImVec2(cx - d.x * 0.5f, tl.y + 154.0f),
                    IM_COL32(180, 162, 122, 200), s);
    }
    // Credit / tagline
    {
        const char* t = "Peel and play.";
        float sz = 15.0f;
        ImVec2 d = font->CalcTextSizeA(sz, FLT_MAX, 0.0f, t);
        bg->AddText(font, sz, ImVec2(cx - d.x * 0.5f, tl.y + 194.0f),
                    IM_COL32(240, 220, 178, 240), t);
    }
    // Esc hint - matches the Shortcuts / Info modals.
    {
        const char* t = "Press Esc to close";
        float sz = 12.0f;
        ImVec2 d = font->CalcTextSizeA(sz, FLT_MAX, 0.0f, t);
        bg->AddText(font, sz, ImVec2(cx - d.x * 0.5f, tl.y + 224.0f),
                    IM_COL32(160, 148, 118, 180), t);
    }

    // License row: shows "Pro - active for <email>" when registered,
    // otherwise an email + key input pair with a Verify button. The
    // whole row lives in its own ImGui window overlaid at a fixed y
    // offset so it can host real inputs (draw list can't).
    {
        ImGuiWindowFlags lflags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                                | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar
                                | ImGuiWindowFlags_NoBackground
                                | ImGuiWindowFlags_NoBringToFrontOnFocus
                                | ImGuiWindowFlags_NoSavedSettings;
        float lic_y = tl.y + 250.0f;
        float lic_h = 140.0f;
        ImGui::SetNextWindowPos(ImVec2(tl.x + 24.0f, lic_y));
        ImGui::SetNextWindowSize(ImVec2(pw - 48.0f, lic_h));
        ImGui::Begin("##AboutLicense", nullptr, lflags);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.35f, 1.0f));
        ImGui::TextUnformatted("License");
        ImGui::PopStyleColor();
        ImGui::Separator();
        ImGui::Spacing();
        License::Status st = License::status();
        if (st == License::Status::Valid) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.72f, 0.90f, 0.60f, 1.0f));
            ImGui::TextUnformatted("Banana Player Pro");
            ImGui::PopStyleColor();
            std::string em = License::activatedEmail();
            ImGui::TextDisabled("registered to %s", em.c_str());
            ImGui::Spacing();
            if (ImGui::SmallButton("Remove license")) {
                License::deactivate();
                license_email_buf_[0] = '\0';
                license_key_buf_[0]   = '\0';
                license_msg_.clear();
            }
        } else {
            ImGui::TextDisabled("Enter the email + key you received after purchase.");
            ImGui::Spacing();
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##lic_email", "your@email.com",
                                     license_email_buf_,
                                     sizeof(license_email_buf_));
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputTextWithHint("##lic_key", "V4-XXXX-XXXX-XXXX",
                                     license_key_buf_,
                                     sizeof(license_key_buf_));
            if (ImGui::Button("Verify", ImVec2(90.0f, 0.0f))) {
                if (License::activate(license_email_buf_, license_key_buf_)) {
                    license_msg_ = "Pro unlocked. Thank you!";
                } else {
                    license_msg_ = "Invalid email + key combination.";
                }
            }
            if (!license_msg_.empty()) {
                ImGui::SameLine();
                bool ok = (License::status() == License::Status::Valid);
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ok ? ImVec4(0.72f, 0.90f, 0.60f, 1.0f)
                       : ImVec4(1.00f, 0.55f, 0.55f, 1.0f));
                ImGui::TextUnformatted(license_msg_.c_str());
                ImGui::PopStyleColor();
            }
        }
        ImGui::End();
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

    // See drawShortcutsPanel comment - background list so window text sits
    // above the panel rects rather than being overpainted.
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(ImVec2(0, 0), ImVec2((float)W, (float)H),
                      IM_COL32(0, 0, 0, 160));

    const float pw = std::min(760.0f, W - 40.0f);
    const float ph = std::min(720.0f, H - 40.0f);
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
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28, 22));
    ImGui::Begin("##InfoModal", nullptr, flags);

    // Header
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.95f, 1.0f, 1.0f));
    float base_font_sz = ImGui::GetFontSize();
    ImGui::SetWindowFontScale(22.0f / base_font_sz);
    ImGui::TextUnformatted("Media Info");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (!player_->isLoaded()) {
        ImGui::TextDisabled("No file loaded.");
    } else {
        // Column layout: 130 px label column, remainder for values. Long
        // strings (filename, path) wrap inside the value column instead
        // of being clipped at the panel edge.
        const float label_col = 130.0f;
        const float value_x   = label_col + 12.0f;
        const float value_w   = pw - value_x - 44.0f;   // leave right margin

        auto section = [&](const char* title) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.82f, 1.0f, 1.0f));
            ImGui::SetWindowFontScale(15.0f / base_font_sz);
            ImGui::TextUnformatted(title);
            ImGui::SetWindowFontScale(1.0f);
            ImGui::PopStyleColor();
            // Subtle underline separator
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddLine(
                ImVec2(p.x, p.y + 2), ImVec2(p.x + pw - 56, p.y + 2),
                IM_COL32(96, 176, 246, 60), 1.0f);
            ImGui::Dummy(ImVec2(0, 6));
        };
        auto row = [&](const char* k, const std::string& v, bool wrap = false) {
            float y0 = ImGui::GetCursorPosY();
            ImGui::SetCursorPosX(4.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.72f, 0.82f, 0.96f, 0.9f));
            ImGui::TextUnformatted(k);
            ImGui::PopStyleColor();
            ImGui::SetCursorPos(ImVec2(value_x, y0));
            ImGui::PushTextWrapPos(value_x + value_w);
            if (wrap) {
                ImGui::TextUnformatted(v.c_str());
            } else {
                ImGui::TextUnformatted(v.c_str());
            }
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
        };
        auto row_disabled = [&](const char* k, const std::string& v) {
            float y0 = ImGui::GetCursorPosY();
            ImGui::SetCursorPosX(4.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.72f, 0.82f, 0.96f, 0.9f));
            ImGui::TextUnformatted(k);
            ImGui::PopStyleColor();
            ImGui::SetCursorPos(ImVec2(value_x, y0));
            ImGui::TextDisabled("%s", v.c_str());
            ImGui::Spacing();
        };

        // Scroll region so overflow doesn't clip silently.
        ImGui::BeginChild("##info_scroll",
                          ImVec2(0.0f, ph - 120.0f),
                          false,
                          ImGuiWindowFlags_HorizontalScrollbar);

        const std::string& path = player_->getFilename();

        // ---- FILE ----
        section("FILE");
        row("Name", baseName(path.c_str()), true);
        row("Path", path, true);
        // Reveal / copy actions right under the path. Handy for people
        // who need to grab the file for another app or share the
        // location without hunting through Explorer.
        {
            float y0 = ImGui::GetCursorPosY();
            ImGui::SetCursorPos(ImVec2(value_x, y0));
            if (ImGui::SmallButton("Reveal in Explorer")) {
                revealInExplorer(path);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy path")) {
                copyPathToClipboard(path);
                showOsd("Copied path");
            }
            ImGui::Spacing();
        }

        // File size via GetFileAttributesExA - returns "" for URLs which is
        // fine, the row just shows "-".
        std::string size_str = "-";
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad)) {
            uint64_t bytes = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
            char sbuf[64];
            if (bytes >= (1ULL << 30))
                std::snprintf(sbuf, sizeof(sbuf), "%.2f GB  (%llu bytes)",
                              bytes / (double)(1ULL << 30),
                              (unsigned long long)bytes);
            else if (bytes >= (1ULL << 20))
                std::snprintf(sbuf, sizeof(sbuf), "%.1f MB  (%llu bytes)",
                              bytes / (double)(1ULL << 20),
                              (unsigned long long)bytes);
            else if (bytes >= 1024)
                std::snprintf(sbuf, sizeof(sbuf), "%.1f KB  (%llu bytes)",
                              bytes / 1024.0,
                              (unsigned long long)bytes);
            else
                std::snprintf(sbuf, sizeof(sbuf), "%llu bytes",
                              (unsigned long long)bytes);
            size_str = sbuf;
        }
        row("Size", size_str);

        // Cumulative watch stats for this file. Live-inclusive - shows
        // the current session's accum too so the number ticks up while
        // you watch. `sessions` counts how many times the file has been
        // opened; useful for "have I already seen this?" checks.
        {
            PlaybackStats st = getStatsFor(path);
            double total_sec = st.seconds + stats_accum_this_session_;
            int    sessions  = st.sessions > 0 ? st.sessions : 1;
            char watch_buf[128];
            int  hrs = (int)(total_sec / 3600.0);
            int  mns = (int)((total_sec - hrs * 3600) / 60.0);
            int  scs = (int)(total_sec - hrs * 3600 - mns * 60);
            if (hrs > 0) {
                std::snprintf(watch_buf, sizeof(watch_buf),
                              "%dh %dm %ds  x %d sessions",
                              hrs, mns, scs, sessions);
            } else if (mns > 0) {
                std::snprintf(watch_buf, sizeof(watch_buf),
                              "%dm %ds  x %d sessions",
                              mns, scs, sessions);
            } else {
                std::snprintf(watch_buf, sizeof(watch_buf),
                              "%ds  x %d sessions",
                              scs, sessions);
            }
            row("Watched", watch_buf);
        }

        // Duration formatted as H:MM:SS with optional fractional seconds.
        double dur = player_->getDuration();
        char dbuf[64];
        if (dur > 0.0) {
            int h = (int)dur / 3600;
            int m = ((int)dur % 3600) / 60;
            double s = dur - (h * 3600 + m * 60);
            if (h > 0)
                std::snprintf(dbuf, sizeof(dbuf), "%d:%02d:%06.3f  (%.3f s)",
                              h, m, s, dur);
            else
                std::snprintf(dbuf, sizeof(dbuf), "%02d:%06.3f  (%.3f s)",
                              m, s, dur);
            row("Duration", dbuf);
        } else {
            row("Duration", "-");
        }

        int64_t br_bits = player_->getFileBitrate();
        char bbuf[64];
        if (br_bits >= 1000000)
            std::snprintf(bbuf, sizeof(bbuf), "%.2f Mbps", br_bits / 1000000.0);
        else if (br_bits > 0)
            std::snprintf(bbuf, sizeof(bbuf), "%.0f kbps", br_bits / 1000.0);
        else
            std::snprintf(bbuf, sizeof(bbuf), "-");
        row("Bit rate", bbuf);

        // ---- VIDEO ----
        section("VIDEO");
        const char* vc = player_->getVideoCodecName();
        row("Codec", (vc && *vc) ? std::string(vc) : std::string("-"));

        char resbuf[96];
        int vw = player_->getVideoWidth();
        int vh = player_->getVideoHeight();
        if (vw > 0 && vh > 0) {
            // Simple aspect: reduce vw:vh by gcd.
            auto gcd = [](int a, int b){ while (b){ a %= b; std::swap(a, b); } return a; };
            int g = gcd(vw, vh);
            std::snprintf(resbuf, sizeof(resbuf), "%d x %d  (%d:%d)",
                          vw, vh, vw / g, vh / g);
        } else {
            std::snprintf(resbuf, sizeof(resbuf), "-");
        }
        row("Resolution", resbuf);

        double fps = player_->getVideoFrameRate();
        char fbuf[48];
        if (fps > 0.0) std::snprintf(fbuf, sizeof(fbuf), "%.3f fps", fps);
        else           std::snprintf(fbuf, sizeof(fbuf), "-");
        row("Frame rate", fbuf);

        std::string dec = player_->isHardwareDecoding()
                            ? (std::string("Hardware  (") + player_->getHardwareDecoderName() + ")")
                            : std::string("Software");
        row("Decoder", dec);

        {
            auto k = player_->getHdrKind();
            const bool on = player_->isHdrToneMapEnabled();
            std::string hdr_label;
            if (k == VideoPlayer::HdrKind::PQ) {
                hdr_label = on ? "PQ (SMPTE ST 2084) - tone-mapped to SDR BT.709"
                               : "PQ (SMPTE ST 2084) - native passthrough (Shift+H)";
            } else if (k == VideoPlayer::HdrKind::HLG) {
                hdr_label = on ? "HLG (ARIB STD-B67) - tone-mapped to SDR BT.709"
                               : "HLG (ARIB STD-B67) - native passthrough (Shift+H)";
            } else {
                hdr_label = "SDR";
            }
            if (on && k != VideoPlayer::HdrKind::None
                && player_->isHdrHalfResActive()) {
                hdr_label += "  [half-res]";
            }
            row("Dynamic range", hdr_label);
        }

        int rot_intrinsic = player_->getIntrinsicRotation();
        char rbuf[48];
        std::snprintf(rbuf, sizeof(rbuf),
                      "%d deg  (container %d + user %d, mod 360)",
                      (rot_intrinsic + ((int)rotation_) * 90) % 360,
                      rot_intrinsic,
                      ((int)rotation_) * 90);
        row("Rotation", rbuf);

        // ---- AUDIO ----
        section("AUDIO");
        if (player_->hasAudio()) {
            const char* ac = player_->getAudioCodecName();
            row("Codec", (ac && *ac) ? std::string(ac) : std::string("-"));
            char sbuf[48];
            std::snprintf(sbuf, sizeof(sbuf), "%d Hz",
                          player_->getAudioSampleRate());
            row("Sample rate", sbuf);
            int ch = player_->getAudioChannels();
            const char* ch_name = (ch == 1) ? " (Mono)"
                                : (ch == 2) ? " (Stereo)"
                                : (ch == 6) ? " (5.1)"
                                : (ch == 8) ? " (7.1)"
                                :             "";
            char cbuf[48];
            std::snprintf(cbuf, sizeof(cbuf), "%d%s", ch, ch_name);
            row("Channels", cbuf);
            int atc = player_->getAudioTrackCount();
            if (atc > 1) {
                char abuf[48];
                std::snprintf(abuf, sizeof(abuf), "%d / %d",
                              player_->getCurrentAudioTrackIndex() + 1, atc);
                row("Track", abuf);
            }
            {
                auto nm = player_->getNormalizeMode();
                float g = player_->getAutoGain();
                char nbuf[64];
                const char* label = (nm == VideoPlayer::NormalizeMode::Strong) ? "Strong"
                                  : (nm == VideoPlayer::NormalizeMode::Soft)   ? "Soft"
                                                                               : "Off";
                if (nm != VideoPlayer::NormalizeMode::Off && g > 1.001f) {
                    std::snprintf(nbuf, sizeof(nbuf), "%s  (+%.1f dB gain)",
                                  label,
                                  20.0f * std::log10(g));
                } else {
                    std::snprintf(nbuf, sizeof(nbuf), "%s", label);
                }
                row("Normalize", nbuf);
            }
        } else {
            row_disabled("Status", "no audio stream");
        }

        // ---- SUBTITLES ----
        section("SUBTITLES");
        if (subtitles_.empty() && subtitle_tracks_.empty()) {
            row_disabled("Status", "none loaded");
        } else {
            char tbuf[128];
            if (!subtitles_.empty()) {
                std::snprintf(tbuf, sizeof(tbuf), "%d cues  (%s)",
                              (int)subtitles_.size(),
                              subtitles_enabled_ ? "shown" : "hidden");
            } else {
                std::snprintf(tbuf, sizeof(tbuf), "-");
            }
            row("Active", tbuf);
            if (!subtitle_tracks_.empty()) {
                std::snprintf(tbuf, sizeof(tbuf), "%d sidecar file(s)",
                              (int)subtitle_tracks_.size());
                row("Sidecars", tbuf);
            }
        }

        // ---- CHAPTERS ----
        if (!chapters_.empty()) {
            section("CHAPTERS");
            char cbuf[48];
            std::snprintf(cbuf, sizeof(cbuf), "%d", (int)chapters_.size());
            row("Count", cbuf);
        }

        // ---- BOOKMARKS ----
        if (!bookmarks_current_.empty()) {
            section("BOOKMARKS");
            char bbuf2[48];
            std::snprintf(bbuf2, sizeof(bbuf2), "%d", (int)bookmarks_current_.size());
            row("Count", bbuf2);
        }

        // ---- YOUTUBE ----
        section("YOUTUBE");
        YtDlpProbeState yt_pst = yt_resolver_ ? yt_resolver_->probeState()
                                              : YtDlpProbeState::Missing;
        if (yt_pst == YtDlpProbeState::Probing) {
            row_disabled("yt-dlp", "detecting...");
        } else if (yt_pst == YtDlpProbeState::Available) {
            std::string ver  = yt_resolver_->ytDlpVersion();
            std::string ppth = yt_resolver_->ytDlpPath();
            int         age  = yt_resolver_->ytDlpAgeDays();
            char line[256];
            if (!ver.empty() && age >= 0)
                std::snprintf(line, sizeof(line), "%s  (%d days old)",
                              ver.c_str(), age);
            else if (!ver.empty())
                std::snprintf(line, sizeof(line), "%s", ver.c_str());
            else
                std::snprintf(line, sizeof(line), "installed");
            row("Version", line);
            if (!ppth.empty()) row_disabled("Path", ppth);
            if (age > 30) {
                row_disabled("Update", "winget upgrade yt-dlp");
            }
        } else {
            row("yt-dlp", "not installed");
            row_disabled("Install", "winget install yt-dlp");
        }

        ImGui::EndChild();
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

    // See drawShortcutsPanel comment - background list so scrollable child
    // window text lands above the panel rects.
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
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
    // get a bare basename row with no progress bar. Stats (watched
    // seconds + session count) are pulled per row from stats_ so the
    // panel shows everything we know about the user's engagement with
    // each file - not just resume position.
    struct Row {
        std::string path;
        double      pos      = -1.0;
        double      duration = -1.0;
        double      watched  = 0.0;
        int         sessions = 0;
    };
    std::vector<Row> rows;
    rows.reserve(resume_index_.size() + recent_files_.size());
    for (const auto& e : resume_index_) {
        Row r; r.path = e.path; r.pos = e.pos; r.duration = e.duration;
        auto it = stats_.find(e.path);
        if (it != stats_.end()) {
            r.watched  = it->second.seconds;
            r.sessions = it->second.sessions;
        }
        rows.push_back(std::move(r));
    }
    for (const auto& rf : recent_files_) {
        bool covered = false;
        for (const auto& r : rows) {
            if (r.path == rf) { covered = true; break; }
        }
        if (!covered) {
            Row r; r.path = rf;
            auto it = stats_.find(rf);
            if (it != stats_.end()) {
                r.watched  = it->second.seconds;
                r.sessions = it->second.sessions;
            }
            rows.push_back(std::move(r));
        }
    }

    if (rows.empty()) {
        ImGui::TextDisabled("No history yet - open a file to start.");
    } else {
        // Aggregate stats banner - total watched, session count, and
        // file count. Gives the panel a "you've engaged this much"
        // feel at a glance instead of just listing files.
        double total_watched = 0.0;
        int    total_sessions = 0;
        int    finished_files = 0;
        for (const auto& r : rows) {
            total_watched  += r.watched;
            total_sessions += r.sessions;
            if (r.duration > 0.0 && r.pos >= 0.0
                && r.pos >= r.duration - 60.0) {
                ++finished_files;
            }
        }
        auto fmtHM = [](double sec) -> std::string {
            if (sec <= 0.0) return "0m";
            int total_min = (int)(sec / 60.0 + 0.5);
            int hours = total_min / 60;
            int mins  = total_min % 60;
            char buf[64];
            if (hours > 0) std::snprintf(buf, sizeof(buf), "%dh %dm", hours, mins);
            else           std::snprintf(buf, sizeof(buf), "%dm", mins);
            return buf;
        };
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.72f, 0.90f, 1.0f, 0.95f));
        char banner[192];
        std::snprintf(banner, sizeof(banner),
                      "Watched %s across %d sessions  .  %d files (%d finished)",
                      fmtHM(total_watched).c_str(),
                      total_sessions, (int)rows.size(), finished_files);
        ImGui::TextUnformatted(banner);
        ImGui::PopStyleColor();
        ImGui::TextDisabled("Click a row to resume  .  x removes");
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
            // Stats line: total watched time + session count. Only
            // shown when there's real data - a new file with 0
            // sessions gets a plain progress-only row.
            if (r.sessions > 0 || r.watched > 0.5) {
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 0.72f, 0.90f, 0.85f));
                int total_min = (int)(r.watched / 60.0 + 0.5);
                int hours = total_min / 60;
                int mins  = total_min % 60;
                char sbuf[96];
                if (hours > 0) {
                    std::snprintf(sbuf, sizeof(sbuf),
                                  "Watched %dh %dm  .  %d session%s",
                                  hours, mins, r.sessions,
                                  r.sessions == 1 ? "" : "s");
                } else {
                    std::snprintf(sbuf, sizeof(sbuf),
                                  "Watched %dm  .  %d session%s",
                                  mins, r.sessions,
                                  r.sessions == 1 ? "" : "s");
                }
                ImGui::TextUnformatted(sbuf);
                ImGui::PopStyleColor();
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

void UIController::drawGotoTimeDialog(int W, int H) {
    if (!show_goto_time_) return;
    if (!player_->isLoaded()) { show_goto_time_ = false; return; }

    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(ImVec2(0, 0), ImVec2((float)W, (float)H),
                      IM_COL32(0, 0, 0, 120));

    const float pw = 340.0f;
    const float ph = 130.0f;
    ImVec2 tl((W - pw) * 0.5f, (H - ph) * 0.5f);
    ImVec2 br(tl.x + pw, tl.y + ph);
    bg->AddRectFilled(ImVec2(tl.x - 3, tl.y - 3), ImVec2(br.x + 3, br.y + 6),
                      IM_COL32(0, 0, 0, 100), 12.0f);
    bg->AddRectFilled(tl, br, IM_COL32(18, 16, 12, 250), 10.0f);
    bg->AddRect(tl, br, IM_COL32(255, 201, 58, 180), 10.0f, 0, 1.2f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                             | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar
                             | ImGuiWindowFlags_NoBackground
                             | ImGuiWindowFlags_NoSavedSettings;
    ImGui::SetNextWindowPos(tl);
    ImGui::SetNextWindowSize(ImVec2(pw, ph));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 14));
    ImGui::Begin("##GotoModal", nullptr, flags);

    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.35f, 1.0f), "Go to time");
    ImGui::TextDisabled("Format: H:MM:SS  .  MM:SS  .  or seconds");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1.0f);
    ImGuiInputTextFlags itf = ImGuiInputTextFlags_EnterReturnsTrue
                            | ImGuiInputTextFlags_AutoSelectAll;
    bool submitted = false;
    // Grab keyboard focus once when the dialog opens.
    if (goto_focus_pending_) {
        ImGui::SetKeyboardFocusHere();
        goto_focus_pending_ = false;
    }
    if (ImGui::InputText("##goto_time_input",
                         goto_time_buf_, sizeof(goto_time_buf_), itf)) {
        submitted = true;
    }
    // Parse: "H:MM:SS" -> h*3600 + m*60 + s
    //        "MM:SS"   -> m*60 + s
    //        plain     -> raw seconds
    auto parse = [](const char* text, double& out) -> bool {
        if (!text || !*text) return false;
        int h = 0, m = 0, s = 0;
        double raw = 0.0;
        if (std::sscanf(text, "%d:%d:%d", &h, &m, &s) == 3) {
            out = h * 3600.0 + m * 60.0 + s;
            return out >= 0.0;
        }
        if (std::sscanf(text, "%d:%d", &m, &s) == 2) {
            out = m * 60.0 + s;
            return out >= 0.0;
        }
        if (std::sscanf(text, "%lf", &raw) == 1) {
            out = raw;
            return out >= 0.0;
        }
        return false;
    };
    ImGui::Spacing();
    bool go_click = ImGui::Button("Go", ImVec2(76.0f, 0.0f));
    ImGui::SameLine();
    bool cancel_click = ImGui::Button("Cancel", ImVec2(76.0f, 0.0f));
    if (cancel_click || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        show_goto_time_ = false;
        goto_time_buf_[0] = '\0';
    }
    if (submitted || go_click) {
        double t = 0.0;
        if (parse(goto_time_buf_, t)) {
            double d = player_->getDuration();
            if (d > 0.0 && t > d) t = d - 1.0;
            if (t < 0.0) t = 0.0;
            player_->seek(t);
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Goto %s", formatTime(t).c_str());
            showOsd(buf, 1.6f);
            show_goto_time_ = false;
            goto_time_buf_[0] = '\0';
        } else {
            showOsd("Invalid time - try 1:23 or 3600", 1.8f);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void UIController::drawSettingsDialog(int W, int H) {
    if (!show_settings_) return;

    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(ImVec2(0, 0), ImVec2((float)W, (float)H),
                      IM_COL32(0, 0, 0, 160));

    const float pw = std::min(720.0f, W - 60.0f);
    const float ph = std::min(560.0f, H - 60.0f);
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
    ImGui::Begin("##SettingsModal", nullptr, flags);

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.95f, 1.0f, 1.0f));
    ImGui::TextUnformatted("Settings");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    // Helper: a radio-style row that stores int into `slot`.
    auto radio_group = [&](const char* header,
                           int& slot,
                           std::initializer_list<std::pair<const char*, int>> opts,
                           std::function<void(int)> on_change) {
        ImGui::TextUnformatted(header);
        ImGui::Indent(16.0f);
        for (auto& p : opts) {
            bool sel = (slot == p.second);
            if (ImGui::RadioButton(p.first, sel) && !sel) {
                slot = p.second;
                if (on_change) on_change(slot);
                markConfigDirty();
            }
        }
        ImGui::Unindent(16.0f);
        ImGui::Spacing();
    };

    ImGui::BeginChild("##settings_scroll", ImVec2(0.0f, ph - 110.0f),
                      false, ImGuiWindowFlags_None);

    if (ImGui::BeginTabBar("##settings_tabs", ImGuiTabBarFlags_None)) {

        // ---- Playback ----
        if (ImGui::BeginTabItem("Playback")) {
            ImGui::Spacing();
            radio_group("Auto-play next when a file ends (countdown)",
                        auto_next_countdown_,
                        {{"Off",  0}, {"3 sec", 3}, {"5 sec", 5}, {"10 sec", 10}},
                        nullptr);
            if (ImGui::Checkbox("Pause when window loses focus",
                                &pause_on_focus_loss_)) markConfigDirty();
            ImGui::Spacing();
            if (ImGui::Checkbox("Thumbnail preview when hovering seek bar",
                                &thumbnail_preview_enabled_)) markConfigDirty();
            ImGui::EndTabItem();
        }

        // ---- Video ----
        if (ImGui::BeginTabItem("Video")) {
            ImGui::Spacing();
            if (player_) {
                bool tm = player_->isHdrToneMapEnabled();
                if (ImGui::Checkbox("HDR tone map for SDR display (Shift+H)", &tm)) {
                    toggleHdrToneMap();
                }
                ImGui::Spacing();
                bool studio = yuv_studio_range_;
                if (ImGui::Checkbox("Studio YUV range (16-235)  -  off = full range (0-255) default",
                                    &studio)) {
                    yuv_studio_range_ = studio;
                    ++texture_generation_;
                    markConfigDirty();
                }
                ImGui::TextDisabled("Off suits web/social/phone footage. Turn on for DVD/Blu-ray/broadcast rips.");
                ImGui::Spacing();
                radio_group("HDR half-resolution (perf lever for 4K HDR)",
                            hdr_half_res_mode_,
                            {{"Off  (full quality)", 0},
                             {"Auto (on for 4K+ HDR)", 1},
                             {"Force on", 2}},
                            [this](int m) {
                                if (player_) player_->setHdrHalfResMode(
                                    (VideoPlayer::HdrHalfResMode)m);
                            });
                VideoPlayer::DeinterlaceMode dcur = player_->getDeinterlaceMode();
                int di = (dcur == VideoPlayer::DeinterlaceMode::On) ? 2
                       : (dcur == VideoPlayer::DeinterlaceMode::Auto) ? 1 : 0;
                radio_group("Deinterlace (yadif)",
                            di,
                            {{"Off  (default)", 0},
                             {"Auto (when tagged interlaced)", 1},
                             {"On   (always)", 2}},
                            [this](int m) {
                                if (!player_) return;
                                VideoPlayer::DeinterlaceMode dm =
                                    (m == 2) ? VideoPlayer::DeinterlaceMode::On
                                  : (m == 1) ? VideoPlayer::DeinterlaceMode::Auto
                                             : VideoPlayer::DeinterlaceMode::Off;
                                player_->setDeinterlaceMode(dm);
                                // Self-seek so the queued ~8 frames get
                                // dropped and the new mode is visible
                                // on the next displayed frame instead of
                                // ~250 ms later. Matches the main-menu
                                // behavior before the Settings-dialog
                                // migration.
                                if (player_->isLoaded()) {
                                    player_->seekRelative(0.0);
                                }
                            });
            }
            ImGui::EndTabItem();
        }

        // ---- Audio ----
        if (ImGui::BeginTabItem("Audio")) {
            ImGui::Spacing();
            if (player_) {
                VideoPlayer::NormalizeMode ncur = player_->getNormalizeMode();
                int nm = (ncur == VideoPlayer::NormalizeMode::Strong) ? 2
                       : (ncur == VideoPlayer::NormalizeMode::Soft)   ? 1 : 0;
                radio_group("Volume normalize (peak-based static gain)",
                            nm,
                            {{"Off",    0},
                             {"Soft   (peak 0.90)", 1},
                             {"Strong (peak 0.98)", 2}},
                            [this](int m) {
                                if (!player_) return;
                                VideoPlayer::NormalizeMode nn =
                                    (m == 2) ? VideoPlayer::NormalizeMode::Strong
                                  : (m == 1) ? VideoPlayer::NormalizeMode::Soft
                                             : VideoPlayer::NormalizeMode::Off;
                                player_->setNormalizeMode(nn);
                            });
                bool ln = player_->isLoudnessNormalize();
                if (ImGui::Checkbox("Normalizer (loudness, dynaudnorm) (Shift+N)",
                                    &ln)) {
                    player_->setLoudnessNormalize(ln);
                    loudness_normalize_ = ln;
                    markConfigDirty();
                }
                ImGui::TextDisabled("Boost quiet dialogue without clipping loud action.");
                ImGui::Spacing();
                radio_group("Audio output latency (ms)  -  video shift to match ear",
                            audio_out_latency_ms_,
                            {{"0    (exclusive / pro)",       0},
                             {"15   (low-latency headphones)", 15},
                             {"30   (WASAPI shared default)",  30},
                             {"60   (USB DAC)",                60},
                             {"100", 100},
                             {"150", 150},
                             {"200  (Bluetooth typical)",     200}},
                            [this](int v) {
                                if (player_) player_->setAudioOutputLatencyMs(v);
                            });
            }
            ImGui::EndTabItem();
        }

        // ---- Subtitles ----
        if (ImGui::BeginTabItem("Subtitles")) {
            ImGui::Spacing();
            if (ImGui::Checkbox("Enabled (S)", &subtitles_enabled_)) markConfigDirty();
            if (ImGui::Checkbox("Background box (B)", &subtitle_bg_)) markConfigDirty();
            ImGui::Spacing();
            int ss = (int)subtitle_size_;
            radio_group("Size (G to cycle)",
                        ss,
                        {{"Small", 0}, {"Medium", 1}, {"Large", 2}, {"XL", 3}},
                        [this](int v) {
                            subtitle_size_ = (SubtitleSize)v;
                        });
            int sc = (int)subtitle_color_;
            radio_group("Color",
                        sc,
                        {{"White", 0}, {"Yellow", 1}, {"Cyan", 2},
                         {"Sky Blue", 3}, {"Pink", 4}},
                        [this](int v) { subtitle_color_ = (SubColor)v; });
            int so = (int)subtitle_outline_;
            radio_group("Outline",
                        so,
                        {{"Thin", 0}, {"Normal", 1}, {"Thick", 2}},
                        [this](int v) { subtitle_outline_ = (SubOutline)v; });
            radio_group("Font",
                        subtitle_font_,
                        {{"Default (merged atlas)", 0},
                         {"Malgun Gothic Bold",     1},
                         {"Gulim",                  2},
                         {"Batang",                 3}},
                        nullptr);
            ImGui::EndTabItem();
        }

        // ---- Snapshots ----
        if (ImGui::BeginTabItem("Snapshots")) {
            ImGui::Spacing();
            ImGui::TextUnformatted("Folder");
            ImGui::Indent(16.0f);
            const std::string cur = snapshot_dir_.empty()
                                      ? std::string("(next to the app)")
                                      : snapshot_dir_;
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", cur.c_str());
            ImGui::PopTextWrapPos();
            if (ImGui::Button("Choose folder...")) openSnapshotFolderDialog();
            ImGui::SameLine();
            if (ImGui::Button("Open in Explorer")) {
                // Resolve to the effective folder: user override wins,
                // otherwise fall back to whatever takeSnapshot() would
                // use (dataDir - typically %APPDATA%\BananaPlayer). ShellExecute
                // on the folder path pops it up in a new Explorer window.
                std::string dir = snapshot_dir_.empty() ? dataDir() : snapshot_dir_;
                while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) {
                    dir.pop_back();
                }
                if (dir.empty()) {
                    showOsd("No snapshot folder");
                } else {
                    HINSTANCE h = ShellExecuteA(nullptr, "open", dir.c_str(),
                                                nullptr, nullptr, SW_SHOWNORMAL);
                    if ((INT_PTR)h <= 32) showOsd("Explorer open failed");
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset to default")) {
                snapshot_dir_.clear();
                markConfigDirty();
            }
            ImGui::Unindent(16.0f);
            ImGui::Spacing();
            if (ImGui::Checkbox("Include subtitles / HUD in snapshot",
                                &snapshot_include_overlays_)) markConfigDirty();
            ImGui::Spacing();
            radio_group("Burst count",
                        burst_count_,
                        {{"3", 3}, {"5", 5}, {"10", 10}, {"20", 20}},
                        nullptr);
            ImGui::TextUnformatted("Burst interval");
            ImGui::Indent(16.0f);
            struct BI { const char* label; float v; };
            BI bopts[] = {{"0.5 sec", 0.5f}, {"1.0 sec", 1.0f},
                          {"2.0 sec", 2.0f}, {"5.0 sec", 5.0f}};
            for (auto& b : bopts) {
                bool sel = std::fabs(burst_interval_ - b.v) < 0.01f;
                if (ImGui::RadioButton(b.label, sel) && !sel) {
                    burst_interval_ = b.v;
                    markConfigDirty();
                }
            }
            ImGui::Unindent(16.0f);
            ImGui::EndTabItem();
        }

        // ---- Behavior ----
        if (ImGui::BeginTabItem("Behavior")) {
            ImGui::Spacing();
            bool si = single_instance_mode_;
            if (ImGui::Checkbox("Single instance (double-clicked files reuse this window)",
                                &si)) {
                single_instance_mode_ = si;
                markConfigDirty();
                saveConfig();   // persist immediately - see main-menu note
                if (single_instance_mode_) {
                    if (SingleInstance::activate()) {
                        showOsd("Single instance on  (this window is now primary)", 2.6f);
                    } else {
                        showOsd("Single instance on  (another Banana Player is already primary)", 3.0f);
                    }
                } else {
                    SingleInstance::deactivate();
                    showOsd("Single instance off  (each launch opens a new window)", 2.6f);
                }
            }
            ImGui::TextDisabled("When off, each launch opens a new Banana Player window.");
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::EndChild();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    // Bottom row: close button + hint
    if (ImGui::Button("Close")) show_settings_ = false;
    ImGui::SameLine();
    ImGui::TextDisabled("Press Esc or Ctrl+, to close");

    ImGui::End();
    ImGui::PopStyleVar();
}

void UIController::drawSubtitles(int W, int H) {
    if (!subtitles_enabled_) return;
    if (!player_->isLoaded()) return;
    const SubtitleCue* cue = subtitles_.empty()
        ? nullptr
        : currentSubtitle(player_->getCurrentTime());
    const SubtitleCue* cue2 = subtitles_secondary_.empty()
        ? nullptr
        : currentSubtitleSecondary(player_->getCurrentTime());
    // Embedded (container) subtitle track. Renders in the primary slot
    // when no external primary sidecar is loaded. If both an external
    // primary AND an embedded track are active, the external one wins
    // (users who explicitly load a sidecar are expressing a preference).
    SubtitleCue embed_cue_storage;
    if (!cue && player_->getActiveEmbeddedSubtitle() >= 0) {
        std::string embed = player_->getEmbeddedSubtitleAt(
                                player_->getCurrentTime() - subtitle_offset_);
        if (!embed.empty()) {
            embed_cue_storage.text  = std::move(embed);
            embed_cue_storage.start = 0.0;
            embed_cue_storage.end   = 0.0;
            cue = &embed_cue_storage;
        }
    }
    if (!cue && !cue2) return;

    // Pick the subtitle font: user preset if available, fall back to
    // the merged default. GetSubtitleFont returns nullptr when the
    // preset's TTF wasn't found on the system.
    ImFont* font = Style::GetSubtitleFont(
                        (Style::SubtitleFont)subtitle_font_);
    if (!font) font = ImGui::GetFont();
    float base_px = subtitleBasePx(W, H);

    auto split = [](const std::string& text, std::vector<std::string>& out) {
        out.clear();
        std::string cur;
        for (char c : text) {
            if (c == '\n') { out.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        if (!cur.empty()) out.push_back(cur);
    };

    // Split PRIMARY into physical lines and measure each.
    std::vector<std::string> lines;
    if (cue) split(cue->text, lines);

    // Secondary lines - rendered at ~85% of primary size, in a slightly
    // dimmer color, sitting on a separate row BELOW the primary block.
    std::vector<std::string> lines2;
    if (cue2) split(cue2->text, lines2);
    float base_px2 = base_px * 0.85f;

    // Draw stacked from the bottom up, above the bottom overlay area.
    // subtitle_vpos_ shifts the block vertically as a fraction of H:
    // positive raises it, negative lowers it (clamped to keep the block
    // inside the picture area). The secondary block sits at the bottom;
    // primary sits ABOVE it (traditional dual-sub layout: native language
    // on top, target language below - matches how PotPlayer and MX
    // Player present dual subs).
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    float line_h    = base_px  * 1.15f;
    float line_h2   = base_px2 * 1.15f;
    float bottom_pad = 120.0f;
    float vshift    = std::clamp(subtitle_vpos_, -0.15f, 0.60f) * (float)H;
    float block_h   = line_h  * (float)lines.size();
    float block_h2  = line_h2 * (float)lines2.size();
    float gap       = (block_h > 0.0f && block_h2 > 0.0f) ? line_h * 0.25f : 0.0f;

    float sec_y_top = (float)H - bottom_pad - block_h2 - vshift;
    float pri_y_top = sec_y_top - gap - block_h;
    if (pri_y_top < 8.0f) { pri_y_top = 8.0f; sec_y_top = pri_y_top + block_h + gap; }

    // Text color: user preset. Secondary is drawn at 80% intensity so
    // the eye picks the primary as the main line even when both are
    // present. Alpha stays at ~245 on primary for solid readability.
    auto colorForPreset = [](SubColor c, int alpha) -> ImU32 {
        switch (c) {
            case SubColor::Yellow:  return IM_COL32(255, 236, 120, alpha);
            case SubColor::Cyan:    return IM_COL32(140, 240, 235, alpha);
            case SubColor::SkyBlue: return IM_COL32(148, 202, 255, alpha);
            case SubColor::Pink:    return IM_COL32(255, 190, 220, alpha);
            case SubColor::White:
            default:                return IM_COL32(255, 255, 255, alpha);
        }
    };
    ImU32 sh_col  = IM_COL32(  0,   0,   0, 235);
    ImU32 pri_col = colorForPreset(subtitle_color_, 245);
    ImU32 sec_col = colorForPreset(subtitle_color_, 200);

    // Outline radius. Thin keeps the historical 1-px 8-neighbor look;
    // Normal doubles to 2 px; Thick to 3 px. The passes count grows
    // (~(2r+1)^2 - 1), but subtitle text is tiny relative to per-frame
    // work so even 24 extra AddText calls at Thick is imperceptible.
    int outline_r = (subtitle_outline_ == SubOutline::Thick)  ? 3
                  : (subtitle_outline_ == SubOutline::Normal) ? 2
                                                              : 1;

    auto drawLines = [&](const std::vector<std::string>& ls, float top,
                          float px, float lh, ImU32 fg) {
        float y = top;
        for (const auto& s : ls) {
            ImVec2 sz = font->CalcTextSizeA(px, FLT_MAX, 0.0f, s.c_str());
            float x = ((float)W - sz.x) * 0.5f;
            for (int dx = -outline_r; dx <= outline_r; ++dx) {
                for (int dy = -outline_r; dy <= outline_r; ++dy) {
                    if (dx == 0 && dy == 0) continue;
                    dl->AddText(font, px, ImVec2(x + dx, y + dy), sh_col, s.c_str());
                }
            }
            dl->AddText(font, px, ImVec2(x, y), fg, s.c_str());
            y += lh;
        }
    };

    // Optional background box: one rounded rect that spans the widest
    // line's width plus padding, encompassing BOTH primary and secondary
    // blocks so the pair reads as one unit. Drawn first.
    if (subtitle_bg_) {
        float widest = 0.0f;
        for (const auto& s : lines) {
            ImVec2 sz = font->CalcTextSizeA(base_px, FLT_MAX, 0.0f, s.c_str());
            if (sz.x > widest) widest = sz.x;
        }
        for (const auto& s : lines2) {
            ImVec2 sz = font->CalcTextSizeA(base_px2, FLT_MAX, 0.0f, s.c_str());
            if (sz.x > widest) widest = sz.x;
        }
        float pad_x = base_px * 0.5f;
        float pad_y = base_px * 0.20f;
        float top_y = (block_h > 0.0f) ? pri_y_top : sec_y_top;
        float bot_y = (block_h2 > 0.0f) ? (sec_y_top + block_h2) : (pri_y_top + block_h);
        ImVec2 tl(((float)W - widest) * 0.5f - pad_x, top_y - pad_y);
        ImVec2 br(((float)W + widest) * 0.5f + pad_x, bot_y + pad_y);
        dl->AddRectFilled(tl, br, IM_COL32(0, 0, 0, 170), base_px * 0.20f);
    }

    if (block_h  > 0.0f) drawLines(lines,  pri_y_top, base_px,  line_h,  pri_col);
    if (block_h2 > 0.0f) drawLines(lines2, sec_y_top, base_px2, line_h2, sec_col);
}

void UIController::freeBitmapSubtitleTexture() {
    if (sub_bitmap_tex_) {
        SDL_DestroyTexture(sub_bitmap_tex_);
        sub_bitmap_tex_ = nullptr;
    }
    sub_bitmap_tex_id_ = 0;
    sub_bitmap_tex_w_  = 0;
    sub_bitmap_tex_h_  = 0;
}

void UIController::drawBitmapSubtitle(int W, int H) {
    if (!subtitles_enabled_) { freeBitmapSubtitleTexture(); return; }
    if (!player_ || !player_->isLoaded() || !renderer_) {
        freeBitmapSubtitleTexture(); return;
    }
    if (player_->getActiveEmbeddedSubtitle() < 0) {
        freeBitmapSubtitleTexture(); return;
    }
    auto v = player_->getEmbeddedBitmapSubtitleAt(
                    player_->getCurrentTime() - subtitle_offset_);
    if (v.id == 0 || v.w <= 0 || v.h <= 0 || v.rgba.empty()) {
        freeBitmapSubtitleTexture(); return;
    }
    // Rebuild the SDL_Texture when the cue changes (id delta) or the
    // dimensions differ from the cached one. Same-cue frames just
    // reuse the previous upload.
    if (v.id != sub_bitmap_tex_id_
        || v.w  != sub_bitmap_tex_w_
        || v.h  != sub_bitmap_tex_h_
        || !sub_bitmap_tex_) {
        freeBitmapSubtitleTexture();
        sub_bitmap_tex_ = SDL_CreateTexture(renderer_,
            SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, v.w, v.h);
        if (!sub_bitmap_tex_) return;
        SDL_SetTextureBlendMode(sub_bitmap_tex_, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(sub_bitmap_tex_, SDL_SCALEMODE_LINEAR);
        SDL_UpdateTexture(sub_bitmap_tex_, nullptr,
                          v.rgba.data(), v.w * (int)sizeof(uint32_t));
        sub_bitmap_tex_id_ = v.id;
        sub_bitmap_tex_w_  = v.w;
        sub_bitmap_tex_h_  = v.h;
    }
    // Scale from source-video pixel space to on-screen. Approximation:
    // fit the video area at the window's aspect assuming no user zoom
    // or pan (bitmap subs already carry their own positioning). Users
    // with heavy zoom / pan can turn off embedded subs and use a text
    // sidecar instead.
    if (v.video_w <= 0 || v.video_h <= 0) return;
    float src_aspect = (float)v.video_w / (float)v.video_h;
    float win_aspect = (float)W / (float)H;
    float box_w, box_h, box_x, box_y;
    if (win_aspect > src_aspect) {
        box_h = (float)H;
        box_w = H * src_aspect;
        box_x = ((float)W - box_w) * 0.5f;
        box_y = 0.0f;
    } else {
        box_w = (float)W;
        box_h = W / src_aspect;
        box_x = 0.0f;
        box_y = ((float)H - box_h) * 0.5f;
    }
    float sx = box_w / (float)v.video_w;
    float sy = box_h / (float)v.video_h;
    // Use ImGui's foreground draw list so the bitmap sits on top of
    // the video texture (which is drawn by main.cpp's RenderTexture
    // before ImGui's own pass runs). ImTextureID is directly the
    // SDL_Texture* on the SDLRenderer3 backend.
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 tl(box_x + v.x * sx,           box_y + v.y * sy);
    ImVec2 br(box_x + (v.x + v.w) * sx,   box_y + (v.y + v.h) * sy);
    dl->AddImage((ImTextureID)(intptr_t)sub_bitmap_tex_, tl, br);
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

    // Countdown branch: only when there's actually a next item to go to
    // (peek without mutating the playlist) AND the user hasn't set the
    // countdown to 0 (immediate). Repeat One skips the countdown (the
    // same file just replays; nothing to preview).
    bool has_next_target =
        playlist_.hasNext()
     || (playlist_.repeatMode() == Playlist::RepeatMode::All
         && playlist_.size() > 1);
    if (auto_next_countdown_ > 0
        && has_next_target
        && playlist_.repeatMode() != Playlist::RepeatMode::One) {
        auto_next_active_ = true;
        auto_next_secs_   = (float)auto_next_countdown_;
        return;
    }
    handleNaturalEndImmediate();
}

void UIController::handleNaturalEndImmediate() {
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

void UIController::cancelAutoNext() {
    auto_next_active_ = false;
    auto_next_secs_   = 0.0f;
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

    // Chapter announcer: when natural playback crosses into a new
    // chapter, fire a subtle "Chapter: <title>" OSD. Skipped when the
    // user is seek-dragging (their seek OSD is louder anyway) and when
    // the file has no chapters. Announces on the FIRST chapter of a
    // freshly loaded file too - useful when files skip past intro.
    if (loaded && !chapters_.empty() && !seek_dragging_) {
        double t = player_->getCurrentTime();
        int cur_ch = -1;
        for (size_t i = 0; i < chapters_.size(); ++i) {
            if (t + 0.05 >= chapters_[i].start) cur_ch = (int)i;
            else break;
        }
        if (cur_ch >= 0 && cur_ch != last_chapter_announced_) {
            last_chapter_announced_ = cur_ch;
            const auto& ch = chapters_[(size_t)cur_ch];
            char buf[256];
            if (!ch.title.empty()) {
                std::snprintf(buf, sizeof(buf), "Chapter %d: %s",
                              cur_ch + 1, ch.title.c_str());
            } else {
                std::snprintf(buf, sizeof(buf), "Chapter %d", cur_ch + 1);
            }
            showOsd(buf, 2.0f);
        }
    }

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

    // Per-file watch statistics: accumulate dt only while the file is
    // actually playing (not paused). Clamp dt to guard against Windows
    // going to sleep with the window open - a giant dt would add hours
    // of phantom watch time. Committed to `stats_` on file switch / quit.
    if (player_ && player_->isLoaded() && player_->isPlaying()) {
        float clamped = dt;
        if (clamped > 1.0f) clamped = 1.0f;   // ignore stalls/sleeps
        stats_accum_this_session_ += (double)clamped;
    }

    // Auto-next countdown tick. When it hits 0, do the deferred advance.
    if (auto_next_active_) {
        auto_next_secs_ -= dt;
        if (auto_next_secs_ <= 0.0f) {
            auto_next_active_ = false;
            auto_next_secs_   = 0.0f;
            handleNaturalEndImmediate();
        }
    }

    // Burst-snapshot tick: fires one snapshot every burst_interval_
    // seconds until remaining hits zero. Only takes shots while a file
    // is loaded; a natural EOF mid-burst just leaves the counter at
    // whatever it was and stops. Shows one progress OSD per shot
    // ("Burst 3/10") instead of the usual "Saved <path>" - avoids the
    // OSD stack ballooning to 10+ entries during a long burst. The
    // shot itself still gets written; only the toast is suppressed
    // (see takeSnapshot's `burst_remaining_` check).
    if (burst_remaining_ > 0 && player_->isLoaded()) {
        burst_next_at_ -= dt;
        if (burst_next_at_ <= 0.0f) {
            int taken_after = burst_count_ - burst_remaining_ + 1;
            takeSnapshot();
            --burst_remaining_;
            burst_next_at_ = burst_interval_;
            char b[32];
            std::snprintf(b, sizeof(b), "Burst %d/%d",
                          taken_after, burst_count_);
            // Short duration so successive progress toasts replace each
            // other rather than piling up on screen.
            showOsd(b, 0.8f);
            if (burst_remaining_ == 0) {
                showOsd("Burst finished", 1.4f);
            }
        }
    }

    // Deferred single-click play/pause: fires once the short (~220 ms)
    // double-click window has passed without a second click arriving.
    if (pending_video_click_) {
        pending_video_click_timer_ -= dt;
        if (pending_video_click_timer_ <= 0.0f) {
            pending_video_click_ = false;
            pending_video_click_timer_ = 0.0f;
            if (player_->isLoaded()) {
                bool was_playing = player_->isPlaying();
                player_->togglePause();
                showOsd(was_playing ? "Pause" : "Play");
            }
        }
    }

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
    if (loaded) drawBitmapSubtitle(W, H);

    drawShortcutsPanel(W, H);
    drawAboutPanel(W, H);
    drawInfoPanel(W, H);
    drawHistoryPanel(W, H);
    drawSettingsDialog(W, H);
    drawGotoTimeDialog(W, H);
    drawSubtitleSearchPanel(W, H);
    drawFirstRunHint(W, H);
    drawAutoNextOverlay(W, H);

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
    // Base gradient: warm near-black top, deeper warm-brown bottom. Warmer
    // than the previous cool blue-black so the banana wordmark and yellow
    // underline live in the same colour family as their surface.
    Style::GradientRect(dl, ImVec2(0, 0), ImVec2((float)W, (float)H),
                        IM_COL32(24, 20, 14, 255), IM_COL32(6, 5, 3, 255));
    // Decorative colour washes - a warm banana-yellow wash from the
    // top-left and an amber peel-toe wash from the bottom-right for a
    // subtle "spotlight" feel that echoes the brand palette.
    float rmax = std::max((float)W, (float)H);
    dl->AddCircleFilled(ImVec2(0, 0),
                        rmax * 0.60f, IM_COL32(255, 201,  58, 30), 96);
    dl->AddCircleFilled(ImVec2((float)W, (float)H),
                        rmax * 0.55f, IM_COL32(227, 128,  40, 24), 96);
    // A hairline horizontal accent glow through the vertical midline. This
    // is the "premium dark landing page" trick every SaaS marketing team
    // ships. Yellow now that we're a warm-palette brand.
    {
        float ay = H * 0.34f;
        dl->AddRectFilledMultiColor(
            ImVec2(0.0f,     ay),
            ImVec2((float)W, ay + 1.0f),
            IM_COL32(255, 201,  58,  0),
            IM_COL32(255, 201,  58, 55),
            IM_COL32(255, 201,  58, 55),
            IM_COL32(255, 201,  58,  0));
    }

    float cx = W * 0.5f;
    // Push the hero content up a hair so the recents list gets vertical air.
    float cy = H * 0.34f;
    ImFont* font = ImGui::GetFont();

    // --- Wordmark: large "Banana Player" with a small banana glyph on the
    // left, gradient wash + subtle glow beneath. The wordmark is 13 chars
    // (vs the old 4-char "VELA"), so the type-size clamp is a chunk smaller
    // to survive the 640 px minimum window width without wrapping/clipping.
    const char* wordmark = "Banana Player";
    // Scale with window height AND width so the wordmark never runs off
    // either axis. The min of the two clamps means small windows shrink
    // uniformly; wide-tall windows still get a comfortable hero size.
    float ts_by_h = std::clamp((float)H * 0.11f, 34.0f, 72.0f);
    float ts_by_w = std::clamp((float)W * 0.078f, 34.0f, 72.0f);
    float ts = std::min(ts_by_h, ts_by_w);
    ImVec2 td = font->CalcTextSizeA(ts, FLT_MAX, 0.0f, wordmark);
    // Slightly more letter spacing than the old VELA to give "Banana Player"
    // a friendly mark-like rhythm (12 % vs the previous 10 %).
    float letter_gap = ts * 0.02f;
    int   letters    = 0;
    for (const char* p = wordmark; *p; ++p) if (*p != ' ') letters++;
    float total_text_w = td.x + letter_gap * (float)std::max(0, letters - 1);
    // Small banana glyph rendered LEFT of the wordmark so the icon and the
    // word live in one visual unit. Sized proportional to the type height.
    float glyph_r = ts * 0.55f;
    float glyph_gap = ts * 0.32f;
    float total_w = glyph_r * 2.0f + glyph_gap + total_text_w;
    float wm_x = cx - total_w * 0.5f;
    float wm_y = cy - ts * 0.5f;

    // Banana glyph. Same construction as the app icon (crescent from two
    // overlapping ellipse regions) but drawn with ImDrawList primitives so
    // it lives in the vector-only icon rule for in-app art. A tilted arc
    // with a filled convex-poly fallback would be cheaper, but overlapping
    // filled circles + subtract-via-second-circle is what we can express
    // in ImDrawList - so we fake the crescent with a fat yellow arc that
    // reads correctly at splash sizes (>= 34 px).
    {
        ImVec2 gc(wm_x + glyph_r, wm_y + ts * 0.55f);
        float r_out = glyph_r;
        float r_in  = glyph_r * 0.72f;
        float rot   = -0.55f * 3.14159265f;   // ~-100 deg: banana lying horizontal
        // Build the crescent as a thick arc: sweep from 30..210 deg around
        // the center, with the arc STROKED at a big thickness. That reads
        // as a fat banana with tapered ends.
        int seg = 40;
        dl->PathClear();
        for (int i = 0; i <= seg; ++i) {
            float t = (float)i / seg;
            float a = 0.35f * 3.14159265f + t * (2.30f * 3.14159265f - 0.35f * 3.14159265f);
            // Rotate the coordinate frame so the crescent lies horizontal
            // with the concave side pointing down/right (play direction).
            float ca = std::cos(a + rot), sa = std::sin(a + rot);
            float rr = (r_out + r_in) * 0.5f;
            dl->PathLineTo(ImVec2(gc.x + ca * rr, gc.y + sa * rr));
        }
        float stroke = (r_out - r_in);
        // Shadow pass.
        dl->PathStroke(IM_COL32(0, 0, 0, 100), 0, stroke + 3.0f);
        // Peel-toe pass.
        dl->PathClear();
        for (int i = 0; i <= seg; ++i) {
            float t = (float)i / seg;
            float a = 0.35f * 3.14159265f + t * (2.30f * 3.14159265f - 0.35f * 3.14159265f);
            float ca = std::cos(a + rot), sa = std::sin(a + rot);
            float rr = (r_out + r_in) * 0.5f;
            dl->PathLineTo(ImVec2(gc.x + ca * rr, gc.y + sa * rr));
        }
        dl->PathStroke(IM_COL32(227, 154, 28, 255), 0, stroke);
        // Bright body pass (slightly thinner, offset toward the highlight
        // side, so it reads as a gradient without needing per-vertex colours).
        dl->PathClear();
        for (int i = 0; i <= seg; ++i) {
            float t = (float)i / seg;
            float a = 0.35f * 3.14159265f + t * (2.30f * 3.14159265f - 0.35f * 3.14159265f);
            float ca = std::cos(a + rot), sa = std::sin(a + rot);
            float rr = (r_out + r_in) * 0.5f - 1.0f;
            dl->PathLineTo(ImVec2(gc.x + ca * rr - 1.0f, gc.y + sa * rr - 1.0f));
        }
        dl->PathStroke(IM_COL32(255, 201, 58, 255), 0, stroke * 0.75f);
    }

    // Wordmark text.
    {
        float x = wm_x + glyph_r * 2.0f + glyph_gap;
        for (const char* p = wordmark; *p; ++p) {
            char one[2] = { *p, 0 };
            ImVec2 gd = font->CalcTextSizeA(ts, FLT_MAX, 0.0f, one);
            // Layered shadow: two low-alpha offsets so the wordmark reads
            // "lifted", not "underlined". Kept small so it can't feel cheesy.
            dl->AddText(font, ts, ImVec2(x + 1.5f, wm_y + 5.0f),
                        IM_COL32(0, 0, 0, 90),  one);
            dl->AddText(font, ts, ImVec2(x + 0.5f, wm_y + 2.5f),
                        IM_COL32(0, 0, 0, 60),  one);
            // Faint banana-yellow glow behind the crisp glyph so the type
            // feels tinted rather than pure-white on video.
            dl->AddText(font, ts, ImVec2(x, wm_y + 1.0f),
                        IM_COL32(255, 201, 58, 55), one);
            dl->AddText(font, ts, ImVec2(x, wm_y),
                        IM_COL32(248, 244, 232, 255), one);
            x += gd.x + (*p == ' ' ? 0.0f : letter_gap);
        }
    }

    // Accent underline: horizontal banana-yellow gradient bar under the
    // wordmark, plus a second thinner glow line for the "under-luminance"
    // premium feel. Width tracks the full wordmark including the glyph.
    float bar_y = wm_y + ts + 8.0f;
    float bar_w = total_w * 0.62f;
    float bar_h = 3.0f;
    dl->AddRectFilledMultiColor(
        ImVec2(cx - bar_w * 0.5f, bar_y),
        ImVec2(cx + bar_w * 0.5f, bar_y + bar_h),
        IM_COL32(227, 154,  28,   0),
        IM_COL32(255, 222, 122, 255),
        IM_COL32(255, 222, 122, 255),
        IM_COL32(227, 154,  28,   0));
    // Second, wider, low-alpha glow bar below - a bloom.
    dl->AddRectFilledMultiColor(
        ImVec2(cx - bar_w * 0.75f, bar_y + bar_h + 1.0f),
        ImVec2(cx + bar_w * 0.75f, bar_y + bar_h + 4.0f),
        IM_COL32(255, 201,  58,   0),
        IM_COL32(255, 201,  58,  90),
        IM_COL32(255, 201,  58,  90),
        IM_COL32(255, 201,  58,   0));

    // Tagline: what Banana Player is for. Slightly larger + brighter than
    // the hints so the reader's eye lands on it first after the wordmark.
    const char* tagline = "Peel and play.";
    float tag_size = 17.0f;
    ImVec2 tag_dim = font->CalcTextSizeA(tag_size, FLT_MAX, 0.0f, tagline);
    float tag_y = bar_y + bar_h + 24.0f;
    dl->AddText(font, tag_size, ImVec2(cx - tag_dim.x * 0.5f, tag_y),
                IM_COL32(234, 220, 190, 235), tagline);

    // Hint row (small): common actions. Two lines so the trailing "? for
    // shortcuts" tip has room even at 640 px width.
    const char* subtitle = "Drop a video or image    Ctrl+O to open    Space to play";
    float ss = 13.0f;
    ImVec2 sd = font->CalcTextSizeA(ss, FLT_MAX, 0.0f, subtitle);
    float sub_y = tag_y + tag_size + 12.0f;
    dl->AddText(font, ss, ImVec2(cx - sd.x * 0.5f, sub_y),
                IM_COL32(185, 170, 138, 210), subtitle);

    const char* subtitle2 = "?  or  F1  for keyboard shortcuts";
    ImVec2 sd2 = font->CalcTextSizeA(ss, FLT_MAX, 0.0f, subtitle2);
    float sub2_y = sub_y + ss + 6.0f;
    dl->AddText(font, ss, ImVec2(cx - sd2.x * 0.5f, sub2_y),
                IM_COL32(165, 148, 118, 210), subtitle2);

    // Two buttons side-by-side: "Open File..." (primary, filled) and
    // "Paste URL" (secondary). Paste is the fast path for YouTube /
    // direct HTTP links since Ctrl+V from the splash otherwise does
    // nothing visible - the button both discovers the feature and
    // shortcuts the workflow.
    ImVec2 btn_size(220, 46);
    ImVec2 paste_size(150, 46);
    float gap = 12.0f;
    float two_btn_w = btn_size.x + gap + paste_size.x;
    float btn_y = sub2_y + ss + 22.0f;
    ImGui::SetCursorScreenPos(ImVec2(cx - two_btn_w * 0.5f, btn_y));
    if (Style::TextButton("Open File...", btn_size, true)) {
        openFileDialog();
    }
    ImGui::SameLine(0.0f, gap);
    if (Style::TextButton("Paste URL", paste_size, false)) {
        pasteAndPlay();
    }

    // Continue-watching hero: the most recently played file that has a
    // resume position gets a big-target row above the RECENT list, so a
    // returning user can pick up in one click without hunting through a
    // list of similarly-named files. Matches the "Continue" strip every
    // streaming service uses. Only rendered when there's a resume entry
    // with a meaningful playhead (not right at the start or right at the
    // end - same window loadPath uses when deciding whether to restore).
    float post_hero_y = btn_y + btn_size.y + 30.0f;
    ResumeEntry const* hero = nullptr;
    for (const auto& e : resume_index_) {
        if (e.duration > 20.0 && e.pos > 5.0 && e.pos < e.duration - 10.0) {
            hero = &e;
            break;
        }
    }
    if (hero) {
        float hero_top = btn_y + btn_size.y + 24.0f;
        float hero_w   = std::min(560.0f, W * 0.72f);
        float hero_h   = 62.0f;
        float hero_x   = cx - hero_w * 0.5f;
        // Invisible button covers the whole card so click / hover feel
        // native. Drawing happens on top via the foreground list.
        ImGui::SetCursorScreenPos(ImVec2(hero_x, hero_top));
        bool hero_clicked = ImGui::InvisibleButton("##continue_hero",
                                                    ImVec2(hero_w, hero_h));
        bool hero_hov = ImGui::IsItemHovered();
        // Card background: warm dark w/ amber accent bar on the left so it
        // reads as the primary CTA even next to the Open File button.
        ImU32 bg_col   = hero_hov
            ? IM_COL32( 44,  36,  22, 230)
            : IM_COL32( 34,  28,  18, 210);
        ImU32 edge_col = hero_hov
            ? IM_COL32(255, 201,  58, 220)
            : IM_COL32(255, 201,  58, 150);
        dl->AddRectFilled(ImVec2(hero_x, hero_top),
                          ImVec2(hero_x + hero_w, hero_top + hero_h),
                          bg_col, 6.0f);
        dl->AddRect(ImVec2(hero_x, hero_top),
                    ImVec2(hero_x + hero_w, hero_top + hero_h),
                    edge_col, 6.0f, 0, 1.0f);
        dl->AddRectFilled(ImVec2(hero_x, hero_top + 6.0f),
                          ImVec2(hero_x + 4.0f, hero_top + hero_h - 6.0f),
                          IM_COL32(255, 201, 58, 240), 2.0f);
        // Label + filename on top row. Filename truncated to fit.
        float text_x = hero_x + 16.0f;
        dl->AddText(font, 11.0f, ImVec2(text_x, hero_top + 8.0f),
                    IM_COL32(200, 175, 110, 220), "CONTINUE WATCHING");
        const char* bn = baseName(hero->path.c_str());
        char name_buf[256];
        std::snprintf(name_buf, sizeof(name_buf), "%s", bn);
        // Truncate if wider than available room.
        float max_name_w = hero_w - 32.0f - 90.0f;
        ImVec2 nm_dim = font->CalcTextSizeA(17.0f, FLT_MAX, 0.0f, name_buf);
        if (nm_dim.x > max_name_w) {
            // Trim tail chars until it fits, leaving room for the ellipsis.
            int len = (int)std::strlen(name_buf);
            while (len > 4) {
                name_buf[--len] = '\0';
                std::strcpy(name_buf + len - 3, "...");
                ImVec2 d = font->CalcTextSizeA(17.0f, FLT_MAX, 0.0f, name_buf);
                if (d.x <= max_name_w) break;
                name_buf[len - 3] = '\0';
            }
        }
        dl->AddText(font, 17.0f, ImVec2(text_x, hero_top + 22.0f),
                    IM_COL32(245, 235, 210, 240), name_buf);
        // Progress bar + time-left, bottom row.
        float bar_left  = text_x;
        float bar_right = hero_x + hero_w - 100.0f;
        float bar_y2    = hero_top + hero_h - 12.0f;
        dl->AddRectFilled(ImVec2(bar_left, bar_y2),
                          ImVec2(bar_right, bar_y2 + 3.0f),
                          IM_COL32(90, 80, 60, 180), 1.5f);
        double frac = std::clamp(hero->pos / hero->duration, 0.0, 1.0);
        dl->AddRectFilled(
            ImVec2(bar_left, bar_y2),
            ImVec2(bar_left + (float)((bar_right - bar_left) * frac), bar_y2 + 3.0f),
            IM_COL32(255, 201, 58, 235), 1.5f);
        // Time text right-aligned inside the card. Format "M:SS left" if
        // under an hour, "H:MM left" otherwise.
        double left_sec = std::max(0.0, hero->duration - hero->pos);
        char time_buf[32];
        int lh = (int)(left_sec / 3600.0);
        int lm = (int)std::fmod(left_sec / 60.0, 60.0);
        int ls = (int)std::fmod(left_sec, 60.0);
        if (lh > 0) std::snprintf(time_buf, sizeof(time_buf), "%d:%02d left", lh, lm);
        else        std::snprintf(time_buf, sizeof(time_buf), "%d:%02d left", lm, ls);
        ImVec2 td_dim = font->CalcTextSizeA(12.0f, FLT_MAX, 0.0f, time_buf);
        dl->AddText(font, 12.0f,
                    ImVec2(hero_x + hero_w - 16.0f - td_dim.x, hero_top + hero_h - 22.0f),
                    IM_COL32(220, 200, 155, 230), time_buf);
        if (hero_clicked) loadPath(hero->path);
        post_hero_y = hero_top + hero_h + 22.0f;
    }

    // Recent files list. Half-column width scales with the window so the
    // header, divider, and selectable rows all fit inside the 640 minimum.
    if (!recent_files_.empty()) {
        float top = post_hero_y;
        float half_w = std::min(220.0f, W * 0.44f);
        float sel_w  = half_w * 2.0f;
        // Header: uppercase label with an accent dot on the left so it feels
        // like a section marker instead of a floating word.
        const char* label = "RECENT";
        float label_size = 12.0f;
        ImVec2 lsz = font->CalcTextSizeA(label_size, FLT_MAX, 0.0f, label);
        dl->AddCircleFilled(ImVec2(cx - half_w + 3.0f, top + lsz.y * 0.5f),
                            3.0f, IM_COL32(255, 201, 58, 220), 12);
        dl->AddText(font, label_size, ImVec2(cx - half_w + 12.0f, top),
                    IM_COL32(210, 195, 160, 230), label);
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
        // rect - reads more like a modern list row. Warm-brown hover /
        // amber active to sit inside the banana palette.
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.42f, 0.32f, 0.14f, 0.35f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0.68f, 0.50f, 0.18f, 0.55f));
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
                                  IM_COL32(255, 201, 58, 230), 1.5f);
            }
            if (clicked) loadPath(f);
            ImGui::PopID();
            rec_y += 28.0f;
        }
        ImGui::PopStyleColor(2);
    }

    // Promo slot: single-line banner above the version footer. Only
    // ever draws on splash (never during playback), only when the
    // fetcher has cached content, only when not suppressed by a Pro
    // license. Positioned so it stays clear of the recent-files list
    // above and the version footer below.
    if (const Promo::Banner* pb = Promo::current()) {
        float promo_y = (float)H - 62.0f;
        // Don't draw over the recent files list if it happens to reach
        // this low - bumps the promo up so nothing overlaps.
        if (!recent_files_.empty()) {
            float top_recent_bar = 60.0f;   // splash-top offset for RECENT
            float est_bottom = top_recent_bar + 200.0f;
            if (est_bottom + 12.0f > promo_y) promo_y = est_bottom + 12.0f;
        }
        // Compose the display text. Sub-line optional.
        std::string display = pb->headline;
        if (!pb->subline.empty()) display += "   .   " + pb->subline;
        ImFont* pf = ImGui::GetFont();
        float promo_size = 13.0f;
        ImVec2 pd = pf->CalcTextSizeA(promo_size, FLT_MAX, 0.0f, display.c_str());
        float pad_x = 12.0f, pad_y = 5.0f;
        float bw = pd.x + pad_x * 2;
        float bh = pd.y + pad_y * 2;
        float bx = (W - bw) * 0.5f;
        ImVec2 tl(bx, promo_y);
        ImVec2 br(bx + bw, promo_y + bh);
        // Warm accent border to signal the brand's yellow palette
        // without a solid fill that would compete with the wordmark.
        dl->AddRectFilled(tl, br, IM_COL32(28, 22, 12, 200), 8.0f);
        dl->AddRect(tl, br, IM_COL32(255, 201, 58, 150), 8.0f, 0, 1.0f);
        dl->AddText(pf, promo_size, ImVec2(bx + pad_x, promo_y + pad_y),
                    IM_COL32(238, 224, 190, 255), display.c_str());
        // Click surface via InvisibleButton over the drawn area.
        if (!pb->click_url.empty()) {
            ImGui::SetCursorScreenPos(tl);
            ImGui::PushID("##promo_btn");
            ImGui::InvisibleButton("p", ImVec2(bw, bh));
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
            if (ImGui::IsItemClicked()) {
                ShellExecuteA(nullptr, "open", pb->click_url.c_str(),
                              nullptr, nullptr, SW_SHOWNORMAL);
            }
            ImGui::PopID();
        }
    }

    // Version bottom-right. Single source of truth via BP_VERSION. The
    // "v" prefix goes into a slightly brighter tone than the tech stack so
    // the release number reads first at a glance.
    char verhead[24];
    std::snprintf(verhead, sizeof(verhead), "v%s", BP_VERSION);
    const char* stack = "SDL3  +  FFmpeg  +  Dear ImGui";
    float vf_size = 12.0f;
    ImVec2 vh = font->CalcTextSizeA(vf_size, FLT_MAX, 0.0f, verhead);
    ImVec2 vs = font->CalcTextSizeA(vf_size, FLT_MAX, 0.0f, stack);
    float sep = 12.0f;
    float total = vh.x + sep + vs.x;
    float base_x = W - total - 18.0f;
    dl->AddText(font, vf_size, ImVec2(base_x, H - 24.0f),
                IM_COL32(210, 188, 138, 240), verhead);
    dl->AddText(font, vf_size, ImVec2(base_x + vh.x + sep, H - 24.0f),
                IM_COL32(132, 120,  90, 220), stack);

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
    // Time display cycles on click. See time_display_mode_ in the header.
    std::string tstr;
    switch (time_display_mode_) {
        case 1: {
            // Remaining time (accounts for playback rate: at 2x, half the
            // wall time is left; at 0.5x, twice the wall time is left).
            double rem = (double)std::max(0.0f, dur - cur);
            double wall_rem = (pb_rate > 0.01f) ? (rem / pb_rate) : rem;
            tstr = "-" + formatTime(wall_rem);
            break;
        }
        case 2: {
            // Elapsed + when it'll finish on the wall clock. Compact enough
            // to fit next to the seek bar at 640 px.
            double rem = (double)std::max(0.0f, dur - cur);
            double wall_rem = (pb_rate > 0.01f) ? (rem / pb_rate) : rem;
            std::time_t now_t = std::time(nullptr);
            std::tm lt{};
            localtime_s(&lt, &now_t);
            int end_min_total = lt.tm_hour * 60 + lt.tm_min
                              + (int)(wall_rem / 60.0 + 0.5);
            int end_h = (end_min_total / 60) % 24;
            int end_m = end_min_total % 60;
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%s   ends %02d:%02d",
                          formatTime(cur).c_str(), end_h, end_m);
            tstr = buf;
            break;
        }
        default:
            tstr = formatTime(cur) + "  /  " + formatTime(dur);
            break;
    }
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
    ImVec2 seek_rmin = ImGui::GetItemRectMin();
    ImVec2 seek_rmax = ImGui::GetItemRectMax();
    bool   seek_bar_hovered = ImGui::IsItemHovered();
    {
        last_seekbar_rect_ = ImVec4(seek_rmin.x, seek_rmin.y, seek_rmax.x, seek_rmax.y);
    }
    // Lightweight "time under cursor" label above the seek bar. Fires
    // any time the bar is hovered - independent of the thumbnail
    // preview feature (which is opt-in and heavier). Suppressed while
    // dragging so it doesn't fight the seek OSD and while a chapter
    // hover-tooltip is showing.
    if (seek_bar_hovered && !seek_held && dur > 0.0f) {
        ImGuiIO& sb_io = ImGui::GetIO();
        float mx_local = sb_io.MousePos.x;
        if (mx_local >= seek_rmin.x && mx_local <= seek_rmax.x) {
            float t = (mx_local - seek_rmin.x) / (seek_rmax.x - seek_rmin.x) * dur;
            if (t < 0.0f) t = 0.0f; if (t > dur) t = dur;
            std::string ts = formatTime(t);
            ImDrawList* fg_dl = ImGui::GetForegroundDrawList();
            ImVec2 tsz2 = ImGui::CalcTextSize(ts.c_str());
            float lpad = 6.0f;
            float lx = std::clamp(mx_local - tsz2.x * 0.5f - lpad,
                                  seek_rmin.x,
                                  seek_rmax.x - tsz2.x - 2.0f * lpad);
            float ly = seek_rmin.y - tsz2.y - 8.0f;
            // Suppress when thumbnail preview is on - the popup embeds
            // its own time label + frame, so this would be a duplicate.
            if (!thumbnail_preview_enabled_ || !thumb_provider_) {
                fg_dl->AddRectFilled(ImVec2(lx, ly),
                                     ImVec2(lx + tsz2.x + 2.0f * lpad,
                                            ly + tsz2.y + 4.0f),
                                     IM_COL32(20, 24, 34, 220), 3.0f);
                fg_dl->AddText(ImVec2(lx + lpad, ly + 2.0f),
                               IM_COL32(230, 240, 255, 255), ts.c_str());
            }
        }
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
            // Suppress this stand-alone chapter tooltip when the
            // thumbnail preview is active - the thumbnail popup embeds
            // the chapter title itself, so drawing this one on top would
            // be a duplicate.
            if (hover_ch && !(thumbnail_preview_enabled_ && thumb_provider_)) {
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
                char label[160];
                std::snprintf(label, sizeof(label),
                              "Bookmark  %s   (click seek  .  right-click remove)",
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
                // Click actions on the diamond. Diamonds sit BELOW the
                // seek bar (diamond_y = rmax.y + 6), so click-vs-seek
                // never collide. Cursor visual hints at interactivity.
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                double target_t = bookmarks_current_[(size_t)hit];
                if (io.MouseClicked[ImGuiMouseButton_Left]) {
                    player_->seek(target_t);
                    char msg[64];
                    std::snprintf(msg, sizeof(msg), "Bookmark  %s",
                                  formatTime(target_t).c_str());
                    showOsd(msg, 1.4f);
                }
                if (io.MouseClicked[ImGuiMouseButton_Right]) {
                    removeBookmark((size_t)hit);   // OSD fires internally
                }
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
        // Clickable playback-rate pill. Left click cycles common speeds
        // (1.0 -> 1.25 -> 1.5 -> 2.0 -> 0.5 -> 0.75 -> 1.0). Right
        // click resets to 1.0x. Discoverable via cursor + tooltip;
        // hidden entirely when rate is 1.0 so the pill IS the state.
        ImVec2 bpos = ImGui::GetCursorScreenPos();
        ImVec2 bsz  = ImGui::CalcTextSize(rate_buf);
        float bpad_x = 9.0f, bpad_y = 3.5f;
        ImGui::PushID("##rate_pill");
        ImGui::InvisibleButton("r",
            ImVec2(bsz.x + bpad_x * 2, bsz.y + bpad_y * 2));
        bool rate_hovered  = ImGui::IsItemHovered();
        bool rate_clicked  = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        bool rate_rclicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
        if (rate_hovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip("Left click: cycle speed  .  Right click: reset");
        }
        // Draw badge manually at the reserved area (same look as
        // Style::Badge but positioned over the InvisibleButton).
        ImDrawList* rdl = ImGui::GetWindowDrawList();
        ImVec2 rbr(bpos.x + bsz.x + bpad_x * 2, bpos.y + bsz.y + bpad_y * 2);
        float rrounding = (bsz.y + bpad_y * 2) * 0.5f;
        ImU32 pill_bg = rate_hovered
            ? Style::Fade(IM_COL32(80, 118, 200, 240), alpha)
            : Style::Fade(IM_COL32(60,  90, 160, 230), alpha);
        rdl->AddRectFilled(bpos, rbr, pill_bg, rrounding);
        int pa = (pill_bg >> IM_COL32_A_SHIFT) & 0xFF;
        rdl->AddLine(ImVec2(bpos.x + rrounding * 0.6f, bpos.y + 0.5f),
                     ImVec2(rbr.x  - rrounding * 0.6f, bpos.y + 0.5f),
                     IM_COL32(255, 255, 255, std::min(80, pa / 3)), 1.0f);
        rdl->AddText(ImVec2(bpos.x + bpad_x, bpos.y + bpad_y),
                     Style::Fade(IM_COL32(220, 230, 250, 255), alpha),
                     rate_buf);
        if (rate_clicked) {
            // Cycle order picked so common accelerated-watching speeds
            // come first (users hit rate to skim). Reset back to 1.0
            // caps the cycle so the pill disappears on full reset.
            const float cycle[] = {0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f};
            int cur_i = -1;
            for (int k = 0; k < (int)(sizeof(cycle)/sizeof(cycle[0])); ++k) {
                if (std::fabs(cycle[k] - pb_rate) < 0.01f) { cur_i = k; break; }
            }
            float nr = (cur_i < 0) ? 1.0f : cycle[(cur_i + 1) % 6];
            player_->setPlaybackRate(nr);
            char msg[32]; std::snprintf(msg, sizeof(msg), "Speed %.2fx", nr);
            showOsd(msg);
        }
        if (rate_rclicked) {
            player_->setPlaybackRate(1.0f);
            showOsd("Speed 1.00x");
        }
        ImGui::PopID();
        ImGui::SameLine();
    }
    // Clickable time text - cycles the display mode. Use InvisibleButton
    // sized to the text so the click surface exactly matches what the user
    // sees. Hover cursor stays on the fingertip via SetMouseCursor.
    {
        ImVec2 tpos = ImGui::GetCursorScreenPos();
        ImVec2 tsz_now = ImGui::CalcTextSize(tstr.c_str());
        ImGui::PushID("##time_cycle");
        ImGui::InvisibleButton("t", ImVec2(tsz_now.x + 4.0f, tsz_now.y + 4.0f));
        bool time_clicked  = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        bool time_rclicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);
        bool time_hovered  = ImGui::IsItemHovered();
        if (time_hovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip("Left click: cycle display  .  Right click: copy timestamp");
        }
        // Repaint text on top so it sits above the invisible click surface.
        ImDrawList* tdl = ImGui::GetWindowDrawList();
        ImU32 tcol = time_hovered
            ? IM_COL32(255, 224, 130, 255)   // amber-hint on hover
            : IM_COL32(217, 230, 255, 255);
        tdl->AddText(ImVec2(tpos.x + 2.0f, tpos.y + 2.0f),
                     Style::Fade(tcol, alpha), tstr.c_str());
        if (time_clicked) {
            time_display_mode_ = (time_display_mode_ + 1) % 3;
            markConfigDirty();
            const char* label = (time_display_mode_ == 0) ? "Time: elapsed / total"
                              : (time_display_mode_ == 1) ? "Time: remaining"
                                                          : "Time: elapsed + end clock";
            showOsd(label, 1.6f);
        }
        if (time_rclicked) {
            // Copy current playhead as HH:MM:SS to the clipboard.
            // Useful for sharing "look at 12:34" pointers on forums /
            // Discord without recording the whole clip.
            std::string ts = formatTime(cur);
            SDL_SetClipboardText(ts.c_str());
            showOsd(std::string("Copied  ") + ts, 1.4f);
        }
        ImGui::PopID();
    }

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
    // Muted state dims the slider so it visually reads as "silenced" -
    // just the icon change was easy to miss.
    if (muted) ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                                   ImGui::GetStyle().Alpha * 0.45f);
    if (ImGui::SliderFloat("##vol", &vol, 0.0f, 2.0f, "")) {
        player_->setVolume(vol);
        // Dragging the slider while muted implicitly unmutes - the
        // mixer convention. Otherwise the user pulls the fader up
        // and hears nothing which is baffling.
        if (muted) player_->setMuted(false);
        nudgeVisible();
    }
    if (muted) ImGui::PopStyleVar();
    // Right-click on the volume slider resets to 100 %. Universal
    // convention in DAWs and every mixer app - restores unity gain
    // without the user having to drag exactly to the middle tick.
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        player_->setVolume(1.0f);
        if (muted) player_->setMuted(false);
        showOsd("Volume 100%");
        markConfigDirty();
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
        // Normal-range percentage badge that follows the thumb on hover.
        // Complements the AMP pill (shown only in boost region below);
        // gives immediate readout while dragging in the 0-100 % zone.
        bool slider_hovered = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (slider_hovered && vol <= 1.0f) {
            float grab_x = s_min.x + (s_max.x - s_min.x) * (vol / 2.0f);
            char pct[16]; std::snprintf(pct, sizeof(pct), "%d%%",
                                        (int)std::lround(vol * 100.0f));
            ImFont* sfont_h = ImGui::GetFont();
            ImVec2 pd = sfont_h->CalcTextSizeA(11.0f, FLT_MAX, 0.0f, pct);
            float pad = 4.0f;
            ImVec2 ptl(grab_x - pd.x * 0.5f - pad, s_min.y - pd.y - 6.0f);
            ImVec2 pbr(grab_x + pd.x * 0.5f + pad, s_min.y - 3.0f);
            float overflow_r = pbr.x - s_max.x;
            if (overflow_r > 0.0f) { ptl.x -= overflow_r; pbr.x -= overflow_r; }
            float overflow_l = s_min.x - ptl.x;
            if (overflow_l > 0.0f) { ptl.x += overflow_l; pbr.x += overflow_l; }
            sdl_dl->AddRectFilled(ptl, pbr,
                                  Style::Fade(IM_COL32(28, 40, 60, 235), alpha),
                                  (pbr.y - ptl.y) * 0.5f);
            sdl_dl->AddText(sfont_h, 11.0f,
                            ImVec2(ptl.x + pad, ptl.y + 1.0f),
                            Style::Fade(IM_COL32(230, 240, 255, 255), alpha), pct);
        }
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
    // Subtitle on/off toggle - only visible when a subtitle source is
    // available (external sidecar loaded, embedded track active, or
    // secondary track). Otherwise the button would be a dead surface;
    // hiding it keeps the row uncluttered on subtitle-less files.
    bool has_subs = !subtitles_.empty()
                 || !subtitles_secondary_.empty()
                 || (player_ && player_->getActiveEmbeddedSubtitle() >= 0);
    if (has_subs) {
        if (Style::IconButton("subs", Style::Icon::Subtitle, btn_sz,
                              subtitles_enabled_ ? "Subtitles on (S)"
                                                 : "Subtitles off (S)",
                              subtitles_enabled_, 1.0f, alpha)) {
            subtitles_enabled_ = !subtitles_enabled_;
            showOsd(subtitles_enabled_ ? "Subtitles on" : "Subtitles off");
            markConfigDirty();
            nudgeVisible();
        }
        ImGui::SameLine();
    }
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
    // HDR tone-map toggle pill. Only shown when the loaded file is
    // actually HDR - a plain SDR playback should never see it. Text pill
    // instead of an icon because "HDR" is more legible than any glyph
    // we could hand-draw at 34 px, and there's no established icon for
    // "tone map on/off". Active state = tone map engaged (accent blue);
    // inactive = passthrough (translucent). Same accent palette as the
    // rest of the overlay so it doesn't stand out as a mismatch.
    if (player_ && player_->getHdrKind() != VideoPlayer::HdrKind::None) {
        ImGui::SameLine();
        ImVec2 cursor = ImGui::GetCursorScreenPos();
        ImVec2 pill_sz(btn_sz.x + 8.0f, btn_sz.y);
        ImGui::PushID("hdr_toggle");
        bool clicked  = ImGui::InvisibleButton("btn", pill_sz);
        bool hovered  = ImGui::IsItemHovered();
        bool held     = ImGui::IsItemActive();
        ImGui::PopID();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 br(cursor.x + pill_sz.x, cursor.y + pill_sz.y);
        float rounding = pill_sz.y * 0.30f;
        ImU32 bg;
        if (hdr_tonemap_enabled_) {
            bg = held    ? IM_COL32( 64, 132, 216, 210)
               : hovered ? IM_COL32(148, 202, 255, 235)
                         : IM_COL32( 96, 176, 246, 225);
        } else {
            bg = held    ? IM_COL32(255, 255, 255,  55)
               : hovered ? IM_COL32(255, 255, 255,  40)
                         : IM_COL32(  0,   0,   0,   0);
        }
        bg = Style::Fade(bg, alpha);
        if ((bg >> IM_COL32_A_SHIFT) != 0) {
            dl->AddRectFilled(cursor, br, bg, rounding);
        }
        if (!hdr_tonemap_enabled_) {
            // Hairline outline so the OFF state doesn't disappear over
            // dark video. Matches the muted-icon color used at rest.
            dl->AddRect(cursor, br,
                        Style::Fade(IM_COL32(216, 222, 236, 130), alpha),
                        rounding, 0, 1.0f);
        }
        ImU32 fg = hdr_tonemap_enabled_
                       ? Style::Fade(IM_COL32(255, 255, 255, 255), alpha)
                       : Style::Fade(IM_COL32(216, 222, 236, 255), alpha);
        const char* label = "HDR";
        ImVec2 lsz = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(cursor.x + (pill_sz.x - lsz.x) * 0.5f,
                           cursor.y + (pill_sz.y - lsz.y) * 0.5f),
                    fg, label);
        if (hovered) {
            const char* kind_str =
                (player_->getHdrKind() == VideoPlayer::HdrKind::PQ)  ? " PQ"
              : (player_->getHdrKind() == VideoPlayer::HdrKind::HLG) ? " HLG"
                                                                     : "";
            ImGui::SetTooltip("HDR%s tone map: %s  (Shift+H)",
                              kind_str,
                              hdr_tonemap_enabled_ ? "on" : "off");
        }
        if (clicked) {
            toggleHdrToneMap();
            nudgeVisible();
        }
    }
    ImGui::SameLine();
    // Gear icon = Settings dialog directly (Ctrl+, alt). The full main
    // menu (file ops, playlist, per-file bits) is still one right-click
    // away on the video surface, so the icon can be single-purpose.
    if (Style::IconButton("menu", Style::Icon::Settings, btn_sz,
                          "Settings (Ctrl+, / right-click video for menu)",
                          show_settings_, 1.0f, alpha)) {
        show_settings_ = !show_settings_;
        if (show_settings_) {
            show_shortcuts_ = false; show_about_ = false;
            show_info_ = false;      show_history_ = false;
        }
    }
    // Long-standing users still expect the popup menu on this icon -
    // right-click on the icon itself as an escape hatch, in addition
    // to right-clicking on the video area.
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
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

    // Popup menu (right-click on gear icon opens this; left-click opens dialog above)
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

    // Chapter snap: if the cursor is close to a chapter tick, request
    // the thumbnail AT the chapter start (not at the raw cursor time).
    // Makes chapter preview reliable at any cursor precision and
    // pairs the chapter title with the thumbnail below.
    const VideoPlayer::Chapter* snap_ch = nullptr;
    if (!chapters_.empty() && duration > 0.0f) {
        float px_per_sec = (r.z - r.x) / (float)duration;
        const float snap_px = 10.0f;
        for (const auto& ch : chapters_) {
            if (ch.start <= 0.05 || ch.start >= duration) continue;
            float dx = std::fabs(((float)ch.start - (float)t) * px_per_sec);
            if (dx <= snap_px) { snap_ch = &ch; t = ch.start; break; }
        }
    }

    ThumbnailProvider::Result res = thumb_provider_->requestAt(t);

    // Compute popup geometry - even without a texture yet, draw a small
    // "loading..." placeholder so the user sees feedback while the first
    // decode is in flight.
    const float pad = 4.0f;
    // Extra title line when a chapter tick was snapped and it has a name.
    const bool  has_chapter_line = snap_ch && !snap_ch->title.empty();
    const float label_h = ImGui::GetFontSize() + 2.0f
                          + (has_chapter_line ? (ImGui::GetFontSize() + 2.0f) : 0.0f);
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

    // Chapter title (when snapped) above the time label.
    float ly = img_max.y + 2.0f;
    if (has_chapter_line) {
        // Truncate very long titles with an ellipsis so the popup width
        // stays sane. box_w was computed from thumb width so we cap to
        // (box_w - 2*pad) minus a safety margin.
        std::string title = snap_ch->title;
        float max_w = box_w - 2.0f * pad - 6.0f;
        while (!title.empty()) {
            ImVec2 ts = ImGui::CalcTextSize(title.c_str());
            if (ts.x <= max_w) break;
            title.pop_back();
        }
        if (title != snap_ch->title && title.size() > 1) {
            title.replace(title.size() - 1, 1, "...");
        }
        ImVec2 tsz2 = ImGui::CalcTextSize(title.c_str());
        float tx2 = box_x + (box_w - tsz2.x) * 0.5f;
        dl->AddText(ImVec2(tx2, ly),
                    IM_COL32(148, 202, 255, 255), title.c_str());
        ly += ImGui::GetFontSize() + 2.0f;
    }
    // Time label.
    std::string tlabel = formatTime(t);
    ImVec2 lsz = ImGui::CalcTextSize(tlabel.c_str());
    float lx = box_x + (box_w - lsz.x) * 0.5f;
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
        if (ImGui::MenuItem("Find subtitles online...", nullptr, false,
                            player_->isLoaded())) {
            searchSubtitlesOnline();
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
        // Container-embedded subtitle tracks (MKV / MP4). Text-only:
        // SRT / ASS / SSA / mov_text. Bitmap subs (PGS, DVB) show up
        // as disabled entries. Rendered in the primary slot when no
        // external sidecar is loaded.
        {
            auto tracks = player_->getEmbeddedSubtitleTracks();
            if (!tracks.empty()) {
                if (ImGui::BeginMenu("Embedded track (in file)")) {
                    int cur = player_->getActiveEmbeddedSubtitle();
                    if (ImGui::MenuItem("(none)", nullptr, cur < 0)) {
                        player_->setActiveEmbeddedSubtitle(-1);
                        showOsd("Embedded subs off", 1.4f);
                    }
                    for (size_t i = 0; i < tracks.size(); ++i) {
                        const auto& t = tracks[i];
                        char label[256];
                        std::snprintf(label, sizeof(label),
                                      "#%d  %s%s%s%s  [%s]%s",
                                      t.stream_idx,
                                      t.title.empty() ? "" : t.title.c_str(),
                                      t.title.empty() ? "" : "  ",
                                      t.language.empty() ? "" : "(",
                                      t.language.empty() ? "" : t.language.c_str(),
                                      t.codec_name.c_str(),
                                      t.selectable ? "" : "  (unsupported)");
                        bool active = ((int)i == cur);
                        if (ImGui::MenuItem(label, nullptr, active, t.selectable)) {
                            if (player_->setActiveEmbeddedSubtitle((int)i)) {
                                // Mid-playback activation: subtitle packets
                                // that already went past the demuxer were
                                // dropped (no decoder was open then). Seek
                                // slightly back so the decode thread re-
                                // reads the region around the current head
                                // and populates cues covering it. Skip when
                                // near the start (nothing to seek to).
                                double cur_t = player_->getCurrentTime();
                                if (cur_t > 2.0) {
                                    player_->seek(cur_t - 2.0);
                                }
                                char m[256];
                                std::snprintf(m, sizeof(m), "Embedded subs: %s",
                                              t.language.empty() ? t.codec_name.c_str()
                                                                 : t.language.c_str());
                                showOsd(m, 1.8f);
                            } else {
                                showOsd("Embedded subs: cannot open track", 2.0f);
                            }
                        }
                    }
                    ImGui::EndMenu();
                }
            }
        }
        if (subtitle_tracks_.size() > 1) {
            // Secondary track picker for dual-subtitle display. Rendered
            // BELOW the primary line in a smaller, dimmer style - typical
            // learn-a-language layout (native tongue on top, target below).
            if (ImGui::BeginMenu("Secondary track (dual subs)")) {
                if (ImGui::MenuItem("(none)", nullptr,
                                    subtitle_secondary_track_active_ < 0)) {
                    subtitles_secondary_.clear();
                    subtitle_secondary_cache_idx_ = 0;
                    subtitle_secondary_track_active_ = -1;
                    showOsd("Secondary subs off", 1.4f);
                }
                for (size_t i = 0; i < subtitle_tracks_.size(); ++i) {
                    // Skip the one already used as primary - picking the
                    // same file twice is never what the user wants.
                    if ((int)i == subtitle_track_active_) continue;
                    bool active = (int)i == subtitle_secondary_track_active_;
                    if (ImGui::MenuItem(subtitle_tracks_[i].label.c_str(),
                                        nullptr, active)) {
                        if (loadSubtitleSidecarSecondary(
                                subtitle_tracks_[i].path)) {
                            subtitle_secondary_track_active_ = (int)i;
                            char buf[192];
                            std::snprintf(buf, sizeof(buf), "Secondary subs: %s",
                                          subtitle_tracks_[i].label.c_str());
                            showOsd(buf, 1.6f);
                        } else {
                            showOsd("Secondary subs load failed", 2.0f);
                        }
                    }
                }
                ImGui::EndMenu();
            }
        }
        ImGui::Separator();
        {
            // Read-only display of current per-file sync + position - the
            // actual controls are keyboard-only for speed. Styling
            // (size/color/outline/font/background) lives in Settings.
            char sync_buf[64];
            std::snprintf(sync_buf, sizeof(sync_buf),
                          "Sync offset: %+d ms  (Ctrl+Shift+Left/Right)",
                          (int)std::lround(subtitle_offset_ * 1000.0));
            ImGui::MenuItem(sync_buf, nullptr, false, false);
            char vpos_buf[64];
            std::snprintf(vpos_buf, sizeof(vpos_buf),
                          "Position: %+d%%  (Alt+Up/Down)",
                          (int)std::lround(subtitle_vpos_ * 100.0f));
            ImGui::MenuItem(vpos_buf, nullptr, false, false);
        }
        ImGui::Separator();
        // Language-learning helpers: quick clipboard capture + look up
        // the currently-visible line in Google (redirects to whatever
        // dictionary/translation service the user has set as default).
        if (ImGui::MenuItem("Copy current line to clipboard",
                            nullptr, false, player_->isLoaded()
                                             && !(subtitles_.empty()
                                                  && subtitles_secondary_.empty()))) {
            copyCurrentSubtitleToClipboard();
        }
        if (ImGui::MenuItem("Look up current line in browser",
                            nullptr, false, player_->isLoaded()
                                             && !(subtitles_.empty()
                                                  && subtitles_secondary_.empty()))) {
            lookupCurrentSubtitleInBrowser();
        }
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

    // Chapters submenu - only shown when the container actually has
    // chapters. Users can browse and jump by clicking, mark the active
    // one at a glance, and read titles without the seek-bar tick-hover.
    if (!chapters_.empty() && player_->isLoaded()) {
        if (ImGui::BeginMenu("Chapters")) {
            double t = player_->getCurrentTime();
            int active_ch = -1;
            for (size_t i = 0; i < chapters_.size(); ++i) {
                if (t + 0.05 >= chapters_[i].start) active_ch = (int)i;
                else break;
            }
            int target = -1;
            for (size_t i = 0; i < chapters_.size(); ++i) {
                char label[288];
                if (!chapters_[i].title.empty()) {
                    std::snprintf(label, sizeof(label), "%2zu.  %s   [%s]",
                                  i + 1, chapters_[i].title.c_str(),
                                  formatTime(chapters_[i].start).c_str());
                } else {
                    std::snprintf(label, sizeof(label), "%2zu.  Chapter %zu   [%s]",
                                  i + 1, i + 1,
                                  formatTime(chapters_[i].start).c_str());
                }
                if (ImGui::MenuItem(label, nullptr, (int)i == active_ch)) {
                    target = (int)i;
                }
            }
            ImGui::Separator();
            ImGui::TextDisabled("Alt+Left / Alt+Right jumps prev / next");
            ImGui::EndMenu();
            if (target >= 0) {
                player_->seek(chapters_[(size_t)target].start);
                showOsd(chapters_[(size_t)target].title.empty()
                        ? std::string("Chapter ") + std::to_string(target + 1)
                        : std::string("Chapter: ") + chapters_[(size_t)target].title);
            }
        }
    }

    if (ImGui::MenuItem("Snapshot", "Ctrl+S", false, player_->isLoaded())) takeSnapshot();
    {
        bool have_ab = (loop_a_ >= 0.0 && loop_b_ >= 0.0);
        char label[64];
        if (have_ab) {
            std::snprintf(label, sizeof(label),
                          "Save A-B segment...  (%.1fs)",
                          loop_b_ - loop_a_);
        } else {
            std::snprintf(label, sizeof(label),
                          "Save A-B segment...  (set A and B first)");
        }
        if (ImGui::MenuItem(label, nullptr, false,
                            player_->isLoaded() && have_ab)) {
            exportABSegmentToFile();
        }
        char glabel[64];
        if (have_ab) {
            std::snprintf(glabel, sizeof(glabel),
                          "Save A-B as GIF...  (%.1fs, max 20s)",
                          loop_b_ - loop_a_);
        } else {
            std::snprintf(glabel, sizeof(glabel),
                          "Save A-B as GIF...  (set A and B first)");
        }
        if (ImGui::MenuItem(glabel, nullptr, false,
                            player_->isLoaded() && have_ab)) {
            exportABSegmentAsGif();
        }
    }
    if (ImGui::BeginMenu("Burst snapshots (Ctrl+Shift+G)", player_->isLoaded())) {
        char label[64];
        std::snprintf(label, sizeof(label),
                      "Fire now  (%d x %.1fs)", burst_count_, burst_interval_);
        if (ImGui::MenuItem(label, nullptr, false, player_->isLoaded())) {
            triggerBurstSnapshot();
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Count")) {
            for (int n : {5, 10, 20, 50}) {
                char l[16]; std::snprintf(l, sizeof(l), "%d", n);
                if (ImGui::MenuItem(l, nullptr, burst_count_ == n)) {
                    burst_count_ = n;
                    markConfigDirty();
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Interval")) {
            struct I { const char* label; float v; };
            I opts[] = {
                {"0.2 s", 0.2f}, {"0.5 s", 0.5f},
                {"1.0 s", 1.0f}, {"2.0 s", 2.0f},
            };
            for (auto& e : opts) {
                bool active = std::fabs(burst_interval_ - e.v) < 0.001f;
                if (ImGui::MenuItem(e.label, nullptr, active)) {
                    burst_interval_ = e.v;
                    markConfigDirty();
                }
            }
            ImGui::EndMenu();
        }
        if (burst_remaining_ > 0) {
            ImGui::Separator();
            char r[64]; std::snprintf(r, sizeof(r),
                                     "In progress: %d left  (press again to cancel)",
                                     burst_remaining_);
            ImGui::TextDisabled("%s", r);
        }
        ImGui::EndMenu();
    }

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
    if (ImGui::MenuItem("Picture in Picture", "Shift+P", pip_active_)) {
        togglePipMode();
    }
    if (ImGui::MenuItem("HDR tone map (SDR display)", "Shift+H",
                        hdr_tonemap_enabled_)) {
        toggleHdrToneMap();
    }
    ImGui::Separator();

    if (ImGui::MenuItem("Settings...", "Ctrl+,")) {
        show_settings_ = true;
        show_shortcuts_ = false; show_about_ = false;
        show_info_ = false;      show_history_ = false;
    }
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
    if (ImGui::MenuItem("About Banana Player")) {
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
        // Compute total duration by pulling per-file duration from
        // resume_index_ (populated whenever a file has been opened at
        // least once). Files never opened contribute nothing, so we
        // append a "+" when we know the total is incomplete. Useful
        // for "how long will this queue take" at a glance.
        double total_dur = 0.0;
        int    known = 0;
        for (const auto& p : playlist_.items()) {
            for (const auto& e : resume_index_) {
                if (e.path == p && e.duration > 0.0) {
                    total_dur += e.duration;
                    ++known;
                    break;
                }
            }
        }
        ImGui::SameLine();
        char count[64];
        if (known > 0) {
            int total_min = (int)(total_dur / 60.0 + 0.5);
            int h = total_min / 60, m = total_min % 60;
            bool complete = (known == (int)playlist_.size());
            if (h > 0) {
                std::snprintf(count, sizeof(count), "  %zu  .  %dh %dm%s",
                              playlist_.size(), h, m, complete ? "" : "+");
            } else {
                std::snprintf(count, sizeof(count), "  %zu  .  %dm%s",
                              playlist_.size(), m, complete ? "" : "+");
            }
        } else {
            std::snprintf(count, sizeof(count), "  %zu", playlist_.size());
        }
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
        // Watched-status glyph before the row number. Distinguishes
        // never-seen from in-progress from finished at a glance in a
        // long playlist.
        //   [ ]  never opened
        //   [.]  in progress (has resume pos short of end)
        //   [v]  finished (naturally reached end - resume cleared but
        //        stats_ tracked real playing time close to duration)
        // We check stats_ FIRST for the finished case since
        // handleNaturalEnd clears the resume entry when a file plays
        // out naturally; otherwise "finished" would only ever show
        // when the user manually seeked near the end. Falls through
        // to resume_index_ for the in-progress + partially-finished
        // cases.
        const char* status = " ";
        const std::string& item_path = playlist_.items()[i];
        auto si = stats_.find(item_path);
        double item_dur_for_status = 0.0;
        for (const auto& e : resume_index_) {
            if (e.path == item_path) {
                item_dur_for_status = e.duration;
                if (e.duration > 0.0 && e.pos >= e.duration - 60.0) {
                    status = "v";
                } else if (e.pos > 5.0) {
                    status = ".";
                }
                break;
            }
        }
        if (status[0] == ' ' && si != stats_.end() && si->second.sessions > 0) {
            // No resume mark (or resume mark was cleared by natural end).
            // If we have real watch stats, mark as "in progress" or
            // "finished" based on how much of the file was watched.
            if (item_dur_for_status > 0.0
                && si->second.seconds >= item_dur_for_status * 0.90) {
                status = "v";
            } else {
                status = ".";
            }
        }
        char label[48];
        if (is_cur) {
            const char* mark = player_->isPlaying() ? ">> " : "|| ";
            std::snprintf(label, sizeof(label), "%s%s %zu. ", mark, status, i + 1);
        } else {
            std::snprintf(label, sizeof(label), "   %s %zu. ", status, i + 1);
        }
        std::string s = std::string(label) + baseName(playlist_.items()[i].c_str());
        // Append duration in mm:ss when known (pulled from resume_index_ -
        // populated once the file has been opened at least once). Small
        // enough to fit at 260 px panel width without truncating the
        // filename for typical names.
        double item_dur = 0.0;
        for (const auto& e : resume_index_) {
            if (e.path == playlist_.items()[i] && e.duration > 0.0) {
                item_dur = e.duration; break;
            }
        }
        if (item_dur > 0.0) {
            int mins = (int)(item_dur / 60.0);
            int secs = (int)item_dur % 60;
            char dbuf[24];
            std::snprintf(dbuf, sizeof(dbuf), "   [%d:%02d]", mins, secs);
            s += dbuf;
        }
        // Reserve space on the right for the action buttons.
        float avail_w = ImGui::GetContentRegionAvail().x;
        float sel_w   = std::max(60.0f, avail_w - btns_w - 12.0f);
        if (ImGui::Selectable(s.c_str(), is_cur, 0, ImVec2(sel_w, row_h))) {
            play_idx = i;
        }
        // Middle click removes - VLC convention that power users
        // reach for reflexively when the x button feels too small.
        if (ImGui::IsItemHovered()
            && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
            remove_idx = (int)i;
        }
        // Auto-scroll to the current row when the playlist advances
        // (N key / natural end / click). Only fires on the FIRST
        // draw with the new index to avoid fighting user scroll.
        if (is_cur && last_playlist_scroll_idx_ != playlist_.index()) {
            ImGui::SetScrollHereY(0.5f);
            last_playlist_scroll_idx_ = playlist_.index();
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
                (key == SDLK_UP)     || (key == SDLK_DOWN) ||
                (key == SDLK_COMMA)  || (key == SDLK_PERIOD) ||
                (key == SDLK_EQUALS) || (key == SDLK_PLUS) || (key == SDLK_KP_PLUS) ||
                (key == SDLK_MINUS)  || (key == SDLK_KP_MINUS);
            if (!holdable) return;
        }

        nudgeVisible();

        // Hardware media keys - Play/Pause, Stop, Next, Previous.
        // SDL3 translates WM_APPCOMMAND / VK_MEDIA_* from Windows into
        // these keycodes, which fire while the window has keyboard
        // focus. (Truly global-hotkey behavior would need RegisterHotKey
        // + SDL_SetWindowsMessageHook; not covered here.) Handled ahead
        // of the plain-letter shortcuts so nothing else can eat them.
        if (key == SDLK_MEDIA_PLAY_PAUSE || key == SDLK_MEDIA_PLAY) {
            if (player_ && player_->isLoaded()) {
                bool p = player_->isPlaying();
                player_->togglePause();
                showOsd(p ? "Pause" : "Play");
            }
            return;
        }
        if (key == SDLK_MEDIA_STOP) {
            closeCurrent();
            return;
        }
        if (key == SDLK_MEDIA_NEXT_TRACK) {
            playNextInPlaylist();
            return;
        }
        if (key == SDLK_MEDIA_PREVIOUS_TRACK) {
            playPrevInPlaylist();
            return;
        }

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
            if (show_subtitle_search_) { show_subtitle_search_ = false; return; }
            if (show_shortcuts_) { show_shortcuts_ = false; return; }
            if (show_about_)     { show_about_    = false; return; }
            if (show_info_)      { show_info_     = false; return; }
            if (show_history_)   { show_history_  = false; return; }
            if (show_settings_)  { show_settings_ = false; return; }
            if (show_goto_time_) { show_goto_time_ = false; goto_time_buf_[0] = '\0'; return; }
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
        // Settings dialog (Ctrl+,). Convention across most Windows apps.
        // Plain comma stays reserved for backward frame step.
        if (key == SDLK_COMMA && (mod & SDL_KMOD_CTRL)) {
            show_settings_ = !show_settings_;
            if (show_settings_) {
                show_shortcuts_ = false; show_about_ = false;
                show_info_ = false; show_history_ = false;
            }
            return;
        }
        // Ctrl+G: go to a specific timestamp. mpv / VLC convention.
        // Plain G is reserved for subtitle-size cycle.
        if (sc == SDL_SCANCODE_G && (mod & SDL_KMOD_CTRL) && !(mod & SDL_KMOD_SHIFT)) {
            if (player_->isLoaded()) {
                show_goto_time_ = true;
                goto_focus_pending_ = true;
                goto_time_buf_[0] = '\0';
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
        // Ctrl+F - subtitle text search.
        if (sc == SDL_SCANCODE_F && (mod & SDL_KMOD_CTRL) && !(mod & SDL_KMOD_SHIFT)) {
            openSubtitleSearch();
            return;
        }
        // Ctrl+Shift+G - burst-snapshot trigger (N shots at fixed
        // interval, defaults 10x0.5s). Second press mid-burst cancels.
        if (sc == SDL_SCANCODE_G
            && (mod & SDL_KMOD_CTRL) && (mod & SDL_KMOD_SHIFT)) {
            triggerBurstSnapshot();
            return;
        }
        // Ctrl+W closes the currently-loaded file (playlist stays intact
        // so the user can pick something else without re-scanning).
        if (sc == SDL_SCANCODE_W && (mod & SDL_KMOD_CTRL)) {
            closeCurrent();
            return;
        }
        // F4 / Backspace: close current file, back to splash. F4 is the
        // convention users bring in from other players; Backspace is the
        // "back one level" reflex from browsers. Both do the same thing.
        if ((key == SDLK_F4 && !(mod & (SDL_KMOD_ALT | SDL_KMOD_CTRL)))
            || (key == SDLK_BACKSPACE && !(mod & (SDL_KMOD_CTRL | SDL_KMOD_SHIFT | SDL_KMOD_ALT)))) {
            closeCurrent();
            return;
        }
        // Ctrl+Q: quit. Universal desktop-app shortcut for "close the
        // whole thing"; Alt+F4 still works too (OS-level).
        if (sc == SDL_SCANCODE_Q && (mod & SDL_KMOD_CTRL)) {
            if (onExitRequested) onExitRequested();
            return;
        }
        // F5: reload the current file. Re-opens from disk at position 0
        // (same as double-clicking the file again from Explorer).
        // Skipped when nothing loaded to keep the key inert on splash.
        if (key == SDLK_F5 && !(mod & (SDL_KMOD_CTRL | SDL_KMOD_SHIFT | SDL_KMOD_ALT))) {
            if (player_->isLoaded()) {
                std::string p = player_->getFilename();
                if (!p.empty()) {
                    loadPath(p);
                    showOsd("Reloaded");
                }
            }
            return;
        }
        // +/- as volume alternatives (matches most players). Ignores
        // Ctrl/Shift/Alt combos so Ctrl+= (zoom) and shift+= (=+) stay
        // available for other bindings. SDLK_KP_PLUS / KP_MINUS cover
        // the numpad path.
        if (!(mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
            if (key == SDLK_EQUALS || key == SDLK_PLUS || key == SDLK_KP_PLUS) {
                float v = std::min(2.0f, player_->getVolume() + 0.05f);
                player_->setVolume(v);
                char buf[32]; std::snprintf(buf, sizeof(buf), "Volume %d%%", (int)(v * 100));
                showOsd(buf);
                markConfigDirty();
                return;
            }
            if (key == SDLK_MINUS || key == SDLK_KP_MINUS) {
                float v = std::max(0.0f, player_->getVolume() - 0.05f);
                player_->setVolume(v);
                char buf[32]; std::snprintf(buf, sizeof(buf), "Volume %d%%", (int)(v * 100));
                showOsd(buf);
                markConfigDirty();
                return;
            }
        }
        // Ctrl+Home / Ctrl+End: alternate jumps to start/end. Plain
        // Home/End already do this; Ctrl-combos are the "start/end of
        // document" convention from every text editor and browser.
        if (sc == SDL_SCANCODE_HOME && (mod & SDL_KMOD_CTRL)) {
            if (player_->isLoaded()) player_->seek(0.0);
            return;
        }
        if (sc == SDL_SCANCODE_END && (mod & SDL_KMOD_CTRL)) {
            if (player_->isLoaded()) {
                double d = player_->getDuration();
                if (d > 1.0) player_->seek(d - 1.0);
            }
            return;
        }
        // Ctrl+M: add a user bookmark at the current playhead. Per-file,
        // persisted to banana.bookmarks. Duplicate protection: a Ctrl+M
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
        // Shift+P - picture-in-picture toggle. Checked ahead of the
        // plain-P playlist toggle so the modifier doesn't fall through.
        if (sc == SDL_SCANCODE_P
            && (mod & SDL_KMOD_SHIFT)
            && !(mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
            togglePipMode();
            return;
        }
        if (sc == SDL_SCANCODE_P
            && !(mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_SHIFT))) {
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
        // HDR tone-map toggle (Shift+H). Silent when the current file
        // isn't HDR - the toggle still flips but there's nothing to
        // announce, so no OSD noise. Check BEFORE the plain-H Shuffle
        // branch so Shift+H doesn't fall through to it.
        if (sc == SDL_SCANCODE_H
            && (mod & SDL_KMOD_SHIFT)
            && !(mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
            toggleHdrToneMap();
            return;
        }
        // Shuffle toggle (H, no modifiers - Shift is now reserved for
        // the HDR toggle above).
        if (sc == SDL_SCANCODE_H
            && !(mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_SHIFT))) {
            playlist_.setShuffle(!playlist_.shuffle());
            showOsd(playlist_.shuffle() ? "Shuffle on" : "Shuffle off");
            markConfigDirty();
            return;
        }
        // Always-on-top toggle (T, no modifiers).
        if (sc == SDL_SCANCODE_T
            && !(mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_SHIFT))) {
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
        // Frame step. OSD is shown unconditionally (not gated behind
        // NDEBUG like the +/-5s / +/-30s toasts) because a single-frame move
        // is often too small to spot visually - the user needs some
        // acknowledgement that the key press landed. Uses real fps with
        // a 30 fps fallback.
        if (key == SDLK_COMMA) {
            if (player_->isLoaded()) {
                double fps = player_->getVideoFrameRate();
                double step = (fps > 0.0) ? 1.0 / fps : 1.0 / 30.0;
                player_->stepFrame(-step);
                showOsd("< Frame back", 0.9f);
            }
            return;
        }
        if (key == SDLK_PERIOD) {
            if (player_->isLoaded()) {
                double fps = player_->getVideoFrameRate();
                double step = (fps > 0.0) ? 1.0 / fps : 1.0 / 30.0;
                player_->stepFrame( step);
                showOsd("Frame forward >", 0.9f);
            }
            return;
        }
        // Audio normalizer toggle (Shift+N). Ties to loudness_normalize
        // (dynaudnorm in the audio decode graph) - the "make quiet
        // dialogue louder without clipping loud action" one. Peak-based
        // Volume normalize stays in Settings only. Checked BEFORE the
        // plain-N next-in-playlist branch below.
        if (sc == SDL_SCANCODE_N
            && (mod & SDL_KMOD_SHIFT)
            && !(mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT))) {
            if (player_) {
                bool nowOn = !player_->isLoudnessNormalize();
                player_->setLoudnessNormalize(nowOn);
                loudness_normalize_ = nowOn;
                showOsd(nowOn ? "Normalizer on" : "Normalizer off");
                markConfigDirty();
            }
            return;
        }
        // Playlist next/prev. Ctrl+N/Ctrl+B kept for muscle memory; plain N
        // is the shorter standard-player binding when no letter conflicts.
        if (key == SDLK_N && (mod & SDL_KMOD_CTRL))         { playNextInPlaylist(); return; }
        if (key == SDLK_B && (mod & SDL_KMOD_CTRL))         { playPrevInPlaylist(); return; }
        if (sc == SDL_SCANCODE_N
            && !(mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_SHIFT))) {
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
        // Ctrl+A - cycle audio track. Checked BEFORE the plain-A A-B
        // loop toggle so the modifier doesn't fall through to it.
        if (key == SDLK_A && (mod & SDL_KMOD_CTRL) && !(mod & SDL_KMOD_ALT)) {
            cycleAudioTrack();
            return;
        }
        // J - cycle external subtitle sidecar (mpv convention).
        if (key == SDLK_J && !(mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_SHIFT))) {
            cycleSubtitleTrack();
            return;
        }
        // Alt+0 - reset video zoom and pan to defaults.
        if (key == SDLK_0 && (mod & SDL_KMOD_ALT)
            && !(mod & (SDL_KMOD_CTRL | SDL_KMOD_SHIFT))) {
            resetVideoZoomPan();
            showOsd("Zoom 100%  Pan 0", 1.2f);
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
    } else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        // Auto-pause when the window loses keyboard focus. Only touches
        // playback state if the toggle is on AND the video is currently
        // playing - so backgrounding an already-paused clip stays paused.
        // We remember that WE paused it so we can resume on focus regain.
        if (pause_on_focus_loss_ && player_ && player_->isLoaded()
            && player_->isPlaying()) {
            player_->pause();
            paused_by_focus_loss_ = true;
        }
    } else if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
        if (paused_by_focus_loss_ && player_ && player_->isLoaded()
            && !player_->isPlaying()) {
            player_->play();
            // OSD once we're back so the user can attribute the resume
            // to the auto-pause feature (and remember to turn it off if
            // it's not what they wanted).
            showOsd("Resumed (window focus)", 1.4f);
        }
        paused_by_focus_loss_ = false;
    } else if (event.type == SDL_EVENT_DROP_FILE) {
        if (event.drop.data) {
            std::string p = event.drop.data;
            // Dropping a subtitle file onto a loaded video swaps in the
            // sidecar cues without touching playback state - matches VLC.
            if (isSubtitleFile(p) && player_->isLoaded()) {
                if (loadSubtitleSidecar(p)) {
                    // Include cue count in the toast so the user gets a
                    // sanity check that parsing found actual lines.
                    // Zero cues would be a red flag - typically a wrong
                    // encoding fallback or a malformed SRT/SMI file.
                    char buf[192];
                    std::snprintf(buf, sizeof(buf), "Subs  %s  (%d cues)",
                                  baseName(p.c_str()),
                                  (int)subtitles_.size());
                    showOsd(buf, 2.4f);
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
        // Alt+left-drag pan. Accumulates delta from the button-down
        // anchor into video_pan_x_/y_ (normalized to a 0..1 fraction of
        // window size, so the pan value stays sensible across resizes).
        if (video_panning_ && window_) {
            int ww = 1, wh = 1;
            SDL_GetWindowSize(window_, &ww, &wh);
            float dx = (event.motion.x - video_pan_start_x_) / (float)ww;
            float dy = (event.motion.y - video_pan_start_y_) / (float)wh;
            float nx = video_pan_orig_x_ + dx;
            float ny = video_pan_orig_y_ + dy;
            // Cap: keep at least a corner of the picture visible so a
            // user can't lose it entirely off-screen.
            if (nx < -1.5f) nx = -1.5f;  if (nx > 1.5f) nx = 1.5f;
            if (ny < -1.5f) ny = -1.5f;  if (ny > 1.5f) ny = 1.5f;
            video_pan_x_ = nx;
            video_pan_y_ = ny;
        }
        // Click-and-drag window move. Promote the pending anchor to a
        // real drag the first time the cursor travels > 4 px from the
        // button-down point; that promotion also cancels the pending
        // play/pause click so the drag doesn't also pause.
        if (window_drag_pending_ && window_ && !fullscreen_) {
            float dx = event.motion.x - window_drag_mouse_x_;
            float dy = event.motion.y - window_drag_mouse_y_;
            if (!window_dragging_ && (dx*dx + dy*dy) >= 16.0f) {
                window_dragging_ = true;
                pending_video_click_       = false;
                pending_video_click_timer_ = 0.0f;
            }
            if (window_dragging_) {
                // Move the OS window by the cursor delta. Use screen-
                // relative global mouse pos so a fast drag that races
                // the resulting window move (which changes the client
                // origin the next motion event is relative to) still
                // tracks the cursor 1:1. Global position isn't in the
                // motion event, so query it fresh each tick.
                float gx = 0.0f, gy = 0.0f;
                SDL_GetGlobalMouseState(&gx, &gy);
                int new_x = (int)std::lround(gx) - (int)window_drag_mouse_x_;
                int new_y = (int)std::lround(gy) - (int)window_drag_mouse_y_;
                SDL_SetWindowPosition(window_, new_x, new_y);
            }
        }
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        if (event.button.button == SDL_BUTTON_LEFT && video_panning_) {
            video_panning_ = false;
        }
        if (event.button.button == SDL_BUTTON_LEFT) {
            window_drag_pending_ = false;
            window_dragging_     = false;
        }
    } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
        // Ctrl+Wheel zooms the video (over any surface, so it also works
        // while the cursor is on the seek bar or a button). Range 0.5 -
        // 4.0 with a 1.05x per-notch multiplicative step so a few clicks
        // feel proportional. Reset via Alt+0.
        if ((SDL_GetModState() & SDL_KMOD_CTRL) && player_->isLoaded()) {
            float factor = (event.wheel.y > 0 ? 1.05f : (1.0f / 1.05f));
            float z = video_zoom_ * factor;
            if (z < 0.50f) z = 0.50f;
            if (z > 4.00f) z = 4.00f;
            if (z != video_zoom_) {
                video_zoom_ = z;
                char buf[32]; std::snprintf(buf, sizeof(buf), "Zoom %d%%",
                                            (int)std::lround(z * 100.0f));
                showOsd(buf, 1.2f);
            }
            nudgeVisible();
            return;
        }
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
            pending_video_click_ = false;
            pending_video_click_timer_ = 0.0f;
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
        // Alt + left-drag pans the (zoomed) picture. Start point is
        // captured here; the actual delta is applied in MOUSE_MOTION.
        // We suppress the play/pause and fullscreen click behavior for
        // this Alt-modified button-down so the pan doesn't also pause.
        if (event.button.button == SDL_BUTTON_LEFT
            && (SDL_GetModState() & SDL_KMOD_ALT)) {
            video_panning_       = true;
            video_pan_start_x_   = event.button.x;
            video_pan_start_y_   = event.button.y;
            video_pan_orig_x_    = video_pan_x_;
            video_pan_orig_y_    = video_pan_y_;
            pending_video_click_ = false;
            pending_video_click_timer_ = 0.0f;
            return;
        }
        if (event.button.button == SDL_BUTTON_LEFT) {
            if (event.button.clicks >= 2) {
                // Double-click -> fullscreen. Cancel any pending single-
                // click toggle AND any pending / active window-drag
                // anchor so a real double-click ends up as JUST
                // fullscreen. Note: no anchor is recorded on this
                // branch. On a double-click that EXITS fullscreen, the
                // second-click DOWN arrives while the window origin has
                // just changed under us, and event.button.x/y (which
                // was client-relative to the fullscreen surface at
                // (0,0)) numerically equals the global mouse - so if
                // we recorded an anchor here and any pixel of jitter
                // followed before button-up, the drag promotion would
                // compute new_x = global - anchor ~ 0 and snap the
                // window to the top-left corner.
                pending_video_click_ = false;
                pending_video_click_timer_ = 0.0f;
                window_drag_pending_ = false;
                window_dragging_     = false;
                setFullscreen(!fullscreen_);
            } else if (player_->isLoaded()) {
                // Single click: defer the toggle so a follow-up second click
                // can convert this into a "just fullscreen" event without a
                // visible pause/play blip in between. The wait uses Windows'
                // configured double-click threshold BUT capped at 220 ms so
                // users with the default 500 ms setting still feel a snappy
                // click - not the beat-late lag the previous long window
                // produced. Empirically 200 - 250 ms catches virtually all
                // real double-clicks; browsers use similar caps.
                UINT dct_ms = GetDoubleClickTime();
                if (dct_ms < 100) dct_ms = 100;
                if (dct_ms > 220) dct_ms = 220;
                pending_video_click_ = true;
                pending_video_click_timer_ = (float)(dct_ms + 20) / 1000.0f;
            }
            // Record a window-drag anchor ONLY on a genuine single
            // click (clicks == 1). MOUSE_MOTION promotes to a real
            // drag once the cursor moves > ~4 px; that promotion also
            // cancels pending_video_click_ so a drag never fires
            // play/pause. Skipped in fullscreen (SDL_SetWindowPosition
            // is a no-op there anyway) and on any clicks >= 2 event
            // (see the double-click comment above for the fullscreen-
            // exit bug that would otherwise fire).
            if (event.button.clicks == 1 && !fullscreen_ && window_) {
                window_drag_pending_ = true;
                window_dragging_     = false;
                window_drag_mouse_x_ = event.button.x;
                window_drag_mouse_y_ = event.button.y;
                SDL_GetWindowPosition(window_,
                                      &window_drag_win_x_,
                                      &window_drag_win_y_);
            }
        }
    }
}

// -------------- Persistent settings (banana.config) --------------

namespace {

std::string sidecarPath(const char* name) { return dataDir() + name; }
std::string configPath()                   { return dataDir() + "banana.config"; }

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
    // Every extension Banana Player will pick up during a folder scan or
    // Open Folder. Videos AND stills - FFmpeg's image2 demuxer handles
    // PNG / JPEG / BMP / TIFF / WebP as single-frame "videos" and
    // VideoPlayer flags them via isStillImage() so they don't auto-advance
    // in a playlist. Keep in sync with the openFileDialogWin() filter
    // below and the OpenWithList block in resources/banana_reg_install.nsh.
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
    // First-run detection: the config sidecar is created lazily on the
    // first quit. If it doesn't exist, we assume this is a fresh
    // install and enable the welcome banner. The banner then persists
    // `first_run_done=1` so a subsequent launch (with a config now on
    // disk) suppresses it.
    if (fopen_s(&fp, path.c_str(), "rb") != 0 || !fp) {
        first_run_hint_pending_ = true;
        return;
    }
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
        else if (std::strcmp(k, "subtitle_color") == 0) {
            int i = std::atoi(v);
            subtitle_color_ = (i == 1) ? SubColor::Yellow
                            : (i == 2) ? SubColor::Cyan
                            : (i == 3) ? SubColor::SkyBlue
                            : (i == 4) ? SubColor::Pink
                                       : SubColor::White;
        }
        else if (std::strcmp(k, "subtitle_outline") == 0) {
            int i = std::atoi(v);
            subtitle_outline_ = (i == 2) ? SubOutline::Thick
                              : (i == 1) ? SubOutline::Normal
                                         : SubOutline::Thin;
        }
        else if (std::strcmp(k, "subtitle_font") == 0) {
            int i = std::atoi(v);
            if (i < 0) i = 0; else if (i > 3) i = 3;
            subtitle_font_ = i;
        }
        else if (std::strcmp(k, "subtitles_enabled") == 0) subtitles_enabled_ = std::atoi(v) != 0;
        else if (std::strcmp(k, "always_on_top") == 0)   always_on_top_ = std::atoi(v) != 0;
        else if (std::strcmp(k, "hdr_tonemap") == 0)     hdr_tonemap_enabled_ = std::atoi(v) != 0;
        else if (std::strcmp(k, "yuv_studio_range") == 0) yuv_studio_range_ = std::atoi(v) != 0;
        else if (std::strcmp(k, "audio_out_latency_ms") == 0) {
            int i = std::atoi(v);
            if (i < 0) i = 0; else if (i > 500) i = 500;
            audio_out_latency_ms_ = i;
        }
        else if (std::strcmp(k, "loudness_normalize") == 0) {
            loudness_normalize_ = std::atoi(v) != 0;
        }
        else if (std::strcmp(k, "time_display_mode") == 0) {
            int i = std::atoi(v);
            if (i < 0 || i > 2) i = 0;
            time_display_mode_ = i;
        }
        else if (std::strcmp(k, "hdr_half_res") == 0) {
            // 0 = Off (default), 1 = Auto (on for 4K+ HDR), 2 = ForceOn.
            int i = std::atoi(v);
            if (i < 0) i = 0; else if (i > 2) i = 2;
            hdr_half_res_mode_ = i;
        }
        else if (std::strcmp(k, "pause_on_focus_loss") == 0) pause_on_focus_loss_ = std::atoi(v) != 0;
        else if (std::strcmp(k, "snapshot_overlays") == 0) snapshot_include_overlays_ = std::atoi(v) != 0;
        else if (std::strcmp(k, "burst_count") == 0) {
            int i = std::atoi(v); if (i > 0 && i <= 200) burst_count_ = i;
        }
        else if (std::strcmp(k, "burst_interval") == 0) {
            float f = (float)std::atof(v);
            if (f >= 0.05f && f <= 10.0f) burst_interval_ = f;
        }
        else if (std::strcmp(k, "snapshot_dir") == 0) {
            // Values are stored as-is (no quoting) - stop at first
            // newline. Empty string keeps the default (appDir()).
            snapshot_dir_ = v ? v : "";
        }
        else if (std::strcmp(k, "normalize_mode") == 0) {
            int i = std::atoi(v);
            VideoPlayer::NormalizeMode m =
                (i == 2) ? VideoPlayer::NormalizeMode::Strong
              : (i == 1) ? VideoPlayer::NormalizeMode::Soft
                         : VideoPlayer::NormalizeMode::Off;
            if (player_) player_->setNormalizeMode(m);
        }
        else if (std::strcmp(k, "first_run_done") == 0) {
            if (std::atoi(v) != 0) first_run_hint_pending_ = false;
        }
        else if (std::strcmp(k, "auto_next_countdown") == 0) {
            int i = std::atoi(v);
            if (i < 0) i = 0; else if (i > 60) i = 60;
            auto_next_countdown_ = i;
        }
        else if (std::strcmp(k, "single_instance") == 0) {
            single_instance_mode_ = std::atoi(v) != 0;
        }
        else if (std::strcmp(k, "deinterlace_mode") == 0) {
            int i = std::atoi(v);
            VideoPlayer::DeinterlaceMode m =
                (i == 2) ? VideoPlayer::DeinterlaceMode::On
              : (i == 0) ? VideoPlayer::DeinterlaceMode::Off
                         : VideoPlayer::DeinterlaceMode::Auto;
            if (player_) player_->setDeinterlaceMode(m);
        }
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
    if (player_) {
        player_->setHdrToneMapEnabled(hdr_tonemap_enabled_);
        VideoPlayer::HdrHalfResMode m =
            (hdr_half_res_mode_ == 2) ? VideoPlayer::HdrHalfResMode::ForceOn
          : (hdr_half_res_mode_ == 1) ? VideoPlayer::HdrHalfResMode::Auto
                                      : VideoPlayer::HdrHalfResMode::Off;
        player_->setHdrHalfResMode(m);
        player_->setAudioOutputLatencyMs(audio_out_latency_ms_);
        player_->setLoudnessNormalize(loudness_normalize_);
    }
}

void UIController::saveConfig() {
    writeSidecarAtomic(configPath(), [&](std::FILE* fp) {
    std::fprintf(fp, "# Banana Player persistent settings\n");
    std::fprintf(fp, "volume=%.4f\n",           player_->getVolume());
    std::fprintf(fp, "muted=%d\n",              player_->isMuted() ? 1 : 0);
    std::fprintf(fp, "subtitle_size=%d\n",      (int)subtitle_size_);
    std::fprintf(fp, "subtitle_offset=%.4f\n",  subtitle_offset_);
    std::fprintf(fp, "subtitle_vpos=%.3f\n",    subtitle_vpos_);
    std::fprintf(fp, "subtitle_bg=%d\n",        subtitle_bg_ ? 1 : 0);
    std::fprintf(fp, "subtitle_color=%d\n",     (int)subtitle_color_);
    std::fprintf(fp, "subtitle_outline=%d\n",   (int)subtitle_outline_);
    std::fprintf(fp, "subtitle_font=%d\n",      subtitle_font_);
    std::fprintf(fp, "subtitles_enabled=%d\n",  subtitles_enabled_ ? 1 : 0);
    std::fprintf(fp, "always_on_top=%d\n",      always_on_top_ ? 1 : 0);
    std::fprintf(fp, "hdr_tonemap=%d\n",        hdr_tonemap_enabled_ ? 1 : 0);
    std::fprintf(fp, "yuv_studio_range=%d\n",   yuv_studio_range_ ? 1 : 0);
    std::fprintf(fp, "audio_out_latency_ms=%d\n", audio_out_latency_ms_);
    std::fprintf(fp, "time_display_mode=%d\n",  time_display_mode_);
    std::fprintf(fp, "loudness_normalize=%d\n",   loudness_normalize_ ? 1 : 0);
    std::fprintf(fp, "hdr_half_res=%d\n",       hdr_half_res_mode_);
    std::fprintf(fp, "pause_on_focus_loss=%d\n", pause_on_focus_loss_ ? 1 : 0);
    std::fprintf(fp, "auto_next_countdown=%d\n", auto_next_countdown_);
    std::fprintf(fp, "single_instance=%d\n",     single_instance_mode_ ? 1 : 0);
    std::fprintf(fp, "snapshot_overlays=%d\n",   snapshot_include_overlays_ ? 1 : 0);
    std::fprintf(fp, "burst_count=%d\n",         burst_count_);
    std::fprintf(fp, "burst_interval=%.3f\n",    burst_interval_);
    if (!snapshot_dir_.empty()) {
        std::fprintf(fp, "snapshot_dir=%s\n",    snapshot_dir_.c_str());
    }
    {
        int nm = 0;
        if (player_) {
            auto m = player_->getNormalizeMode();
            nm = (m == VideoPlayer::NormalizeMode::Strong) ? 2
               : (m == VideoPlayer::NormalizeMode::Soft)   ? 1
                                                           : 0;
        }
        std::fprintf(fp, "normalize_mode=%d\n", nm);
    }
    // Once we've written a config file, the app has "seen" first run.
    // Emit the flag unconditionally so any subsequent launch skips the
    // welcome banner, regardless of whether the user dismissed it or
    // just started playing something.
    std::fprintf(fp, "first_run_done=1\n");
    {
        int dm = 0;   // Off default
        if (player_) {
            auto m = player_->getDeinterlaceMode();
            dm = (m == VideoPlayer::DeinterlaceMode::On)  ? 2
               : (m == VideoPlayer::DeinterlaceMode::Off) ? 0
                                                          : 1;
        }
        std::fprintf(fp, "deinterlace_mode=%d\n", dm);
    }
    std::fprintf(fp, "repeat_mode=%d\n",        (int)playlist_.repeatMode());
    std::fprintf(fp, "shuffle=%d\n",            playlist_.shuffle() ? 1 : 0);
    std::fprintf(fp, "thumbnail_preview=%d\n",  thumbnail_preview_enabled_ ? 1 : 0);
    std::fprintf(fp, "youtube_quality=%s\n",    youtubeQualityToString(youtube_quality_));
    // NOTE: rotation AND av_sync_offset are deliberately NOT saved -
    // both are per-file transients. See loadConfig for the ignore branches.
    });
    config_dirty_ = false;
}

void UIController::loadRecents() {
    std::string path = sidecarPath("banana.recent");
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
    writeSidecarAtomic(sidecarPath("banana.recent"), [&](std::FILE* fp) {
        std::fprintf(fp, "# Banana Player recent files - most recent first\n");
        for (const auto& p : recent_files_) std::fprintf(fp, "%s\n", p.c_str());
    });
}

void UIController::applyStoredWindowGeometry() {
    std::string path = sidecarPath("banana.window");
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
    std::string path = sidecarPath("banana.window");
    FILE* fp = nullptr;
    if (fopen_s(&fp, path.c_str(), "wb") != 0 || !fp) return;
    std::fprintf(fp, "x=%d\ny=%d\nw=%d\nh=%d\nmaximized=%d\n",
                 x, y, w, h, maximized ? 1 : 0);
    std::fclose(fp);
}

void UIController::loadLastPlaylist() {
    std::string path = sidecarPath("banana.session.m3u");
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
    std::string path = sidecarPath("banana.session.m3u");
    // Wipe the file if the playlist is empty so we don't strand yesterday's
    // list forever.
    if (playlist_.empty()) {
        std::remove(path.c_str());
        return;
    }
    writeSidecarAtomic(path, [&](std::FILE* fp) {
        std::fprintf(fp, "#EXTM3U\n");
        for (const auto& p : playlist_.items()) std::fprintf(fp, "%s\n", p.c_str());
    });
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
