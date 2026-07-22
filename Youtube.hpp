#pragma once

// YoutubeResolver - runs yt-dlp on a worker thread to turn a YouTube page
// URL into a direct media URL that FFmpeg's avformat_open_input can play.
//
// The design mirrors ThumbnailProvider: caller posts a request, drains
// results on the main thread each frame. Only ONE resolve is in flight
// at a time - a new request cancels the previous one (via TerminateProcess
// on the yt-dlp child). Old results are discarded by request-id compare so
// the drain never surfaces a stale URL after the user has moved on.
//
// yt-dlp is NOT bundled with Vela. On construction we probe for it via
// where.exe and also check for a portable yt-dlp.exe next to Vela.exe.
// If neither is found, ytDlpAvailable() returns false and every request
// completes immediately with a "not installed" failure so the caller can
// show install guidance in the OSD.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

// Probe state is a tri-state: not yet run (worker still starting), done +
// available (yt-dlp found), done + missing. Callers that need to distinguish
// "detecting" from "not installed" use probeState() rather than the plain
// ytDlpAvailable() boolean.
enum class YtDlpProbeState {
    Probing = 0,
    Available,
    Missing,
};

class YoutubeResolver {
public:
    struct Result {
        uint64_t    id        = 0;
        std::string source_url;   // original YouTube URL as pasted
        bool        ok        = false;
        std::string media_url;    // direct FFmpeg-openable URL when ok
        std::string message;      // error text for OSD when !ok
    };

    YoutubeResolver();
    ~YoutubeResolver();

    // Non-blocking. Returns a monotonically increasing request id. Calls
    // TerminateProcess on any yt-dlp child from a previous still-in-flight
    // request so only the newest one lands in the drain queue.
    uint64_t requestResolve(const std::string& url);

    // Main-thread poll. Returns true and fills `out` when a resolve
    // (matching or otherwise) has completed since the last drain. The
    // caller should compare out.id to its pending request id and ignore
    // stale results.
    bool tryDrainResult(Result& out);

    // False when neither `where yt-dlp.exe` nor `<vela dir>\yt-dlp.exe`
    // resolved on construction. The caller uses this to distinguish
    // "install yt-dlp" from a normal resolve failure so the OSD text can
    // be actionable. During the brief probe window at startup this returns
    // false too; use probeState() when you need to distinguish "probing".
    bool            ytDlpAvailable() const { return yt_dlp_available_; }
    YtDlpProbeState probeState() const {
        if (!probe_done_.load(std::memory_order_acquire)) return YtDlpProbeState::Probing;
        return yt_dlp_available_ ? YtDlpProbeState::Available
                                 : YtDlpProbeState::Missing;
    }

    // Freshness signals populated at construction. YouTube changes internal
    // protocols (signature ciphering, DASH manifests) every few weeks and
    // yt-dlp releases follow within days - so we surface age and version
    // to the caller who can nudge the user to update. Returns:
    //  - ytDlpAgeDays: exe's LastWriteTime delta in whole days, or -1 if
    //    yt-dlp isn't available / stat failed.
    //  - ytDlpVersion: whatever `yt-dlp --version` printed (usually a date
    //    like "2025.10.22"), or empty on failure / when not installed.
    //  - ytDlpPath: absolute path we resolved, or empty when not installed.
    int         ytDlpAgeDays() const { return yt_dlp_age_days_; }
    std::string ytDlpVersion() const { return yt_dlp_version_; }
    std::string ytDlpPath()    const { return yt_dlp_path_; }

private:
    void workerLoop();

    // Locate yt-dlp.exe on PATH (via where.exe) or next to Vela.exe.
    // Returns empty string on miss. Called once from the ctor.
    static std::string locateYtDlp();

    // Run yt-dlp -g <url>, returning the parsed direct URL on success and
    // filling `err_msg` with a short error blurb on failure. Records the
    // child's process handle into current_child_ so requestResolve can
    // TerminateProcess it if a newer request supersedes this one.
    bool runYtDlp(const std::string& yt_dlp_path,
                  const std::string& url,
                  std::string&       out_media_url,
                  std::string&       err_msg);

    std::string yt_dlp_path_;
    bool        yt_dlp_available_ = false;
    int         yt_dlp_age_days_  = -1;
    std::string yt_dlp_version_;
    // Set once the worker has finished the initial locate+version probe.
    // Ctor stays instant - previously locate + `yt-dlp --version` ran
    // synchronously and added ~500 ms to Vela startup on cold cache.
    std::atomic<bool> probe_done_{false};

    // Producer/consumer queue slot. Only the newest request matters; the
    // worker compares against latest_request_id_ before enqueueing a
    // result so cancelled work never surfaces to the main thread.
    std::mutex              mutex_;
    std::condition_variable cv_;
    std::atomic<bool>       quit_{false};

    bool        has_pending_       = false;
    uint64_t    pending_id_        = 0;
    std::string pending_url_;
    uint64_t    latest_request_id_ = 0;
    uint64_t    next_id_           = 1;

    // Void* to keep <windows.h> out of the header. In practice it's a
    // HANDLE to the yt-dlp process while a resolve is in flight.
    void*       current_child_     = nullptr;

    std::deque<Result> results_;

    std::thread worker_;
};
