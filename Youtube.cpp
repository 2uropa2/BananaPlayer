#include "Youtube.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {

// Full path of the running Vela.exe's directory, used to look for a
// portable yt-dlp.exe dropped next to it.
std::string appDir() {
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return ".";
    std::string s(buf, n);
    size_t p = s.find_last_of("\\/");
    return (p == std::string::npos) ? "." : s.substr(0, p);
}

// Case-insensitive prefix compare.
bool startsWithI(const std::string& s, const char* prefix) {
    size_t n = std::strlen(prefix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        char a = (char)std::tolower((unsigned char)s[i]);
        char b = (char)std::tolower((unsigned char)prefix[i]);
        if (a != b) return false;
    }
    return true;
}

// Read all bytes from a pipe until it's closed by the child. Returns the
// concatenated output. Non-blocking on the far end is not required - the
// caller uses a wait-with-timeout on the process handle.
std::string readAllFromPipe(HANDLE h) {
    std::string out;
    char        buf[4096];
    DWORD       read = 0;
    while (ReadFile(h, buf, sizeof(buf), &read, nullptr) && read > 0) {
        out.append(buf, read);
    }
    return out;
}

// Trim CR/LF/space from both ends of a string.
std::string trimWs(std::string s) {
    auto is_ws = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (!s.empty() && is_ws((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && is_ws((unsigned char)s.back()))  s.pop_back();
    return s;
}

// Take the last N chars of stderr for a compact error OSD. We don't want
// to spam the full multi-line yt-dlp traceback in a toast.
std::string tailError(const std::string& s, size_t max_len = 200) {
    std::string t = trimWs(s);
    // Prefer the last line if we have multiple - the summary is usually
    // there ("ERROR: [youtube] xyz: Video unavailable").
    size_t nl = t.find_last_of("\r\n");
    if (nl != std::string::npos) t = trimWs(t.substr(nl + 1));
    if (t.size() > max_len) t = t.substr(t.size() - max_len);
    return t;
}

} // namespace

namespace {

// Compute whole-day delta between a file's LastWriteTime and now. Returns
// -1 on any failure (missing file, malformed time). We use FileTime deltas
// so DST / timezone shenanigans don't skew the count.
int fileAgeDays(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &a)) return -1;
    FILETIME now_ft{};
    GetSystemTimeAsFileTime(&now_ft);
    ULARGE_INTEGER now_i{};   now_i.LowPart = now_ft.dwLowDateTime;
                              now_i.HighPart = now_ft.dwHighDateTime;
    ULARGE_INTEGER file_i{};  file_i.LowPart = a.ftLastWriteTime.dwLowDateTime;
                              file_i.HighPart = a.ftLastWriteTime.dwHighDateTime;
    if (now_i.QuadPart <= file_i.QuadPart) return 0;
    // FILETIME ticks are 100 ns. Convert to days.
    const uint64_t kTicksPerDay = 10ULL * 1000 * 1000 * 60 * 60 * 24;
    uint64_t delta = now_i.QuadPart - file_i.QuadPart;
    return (int)(delta / kTicksPerDay);
}

// Run `<yt_dlp_path> --version` and return the trimmed stdout. Bounded at
// 2 s because it's synchronous during construction and we don't want to
// hold up UIController for a hung binary.
std::string probeYtDlpVersion(const std::string& yt_dlp_path) {
    if (yt_dlp_path.empty()) return {};
    HANDLE read_h  = nullptr;
    HANDLE write_h = nullptr;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&read_h, &write_h, &sa, 0)) return {};
    SetHandleInformation(read_h, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = write_h;
    si.hStdError  = write_h;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::string cmd = "\"" + yt_dlp_path + "\" --version";
    std::vector<char> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back('\0');
    BOOL ok = CreateProcessA(nullptr, cmd_buf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(write_h);
    if (!ok) { CloseHandle(read_h); return {}; }

    if (WaitForSingleObject(pi.hProcess, 2000) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
    }
    std::string out = readAllFromPipe(read_h);
    CloseHandle(read_h);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    // First non-empty line only. yt-dlp normally prints a single version
    // like "2025.10.22"; anything else is unexpected.
    std::string first = trimWs(out);
    size_t nl = first.find_first_of("\r\n");
    if (nl != std::string::npos) first = trimWs(first.substr(0, nl));
    return first;
}

} // namespace

YoutubeResolver::YoutubeResolver() {
    // Ctor stays instant. The locate + version probe (2 child processes,
    // ~500 ms cold) now runs as the first phase of the worker thread so
    // Vela's startup path - especially the Explorer-double-click case
    // where UIController is constructed before the first frame draws -
    // doesn't stall waiting for where.exe + `yt-dlp --version`.
    worker_ = std::thread(&YoutubeResolver::workerLoop, this);
}

YoutubeResolver::~YoutubeResolver() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        quit_ = true;
        // If the worker is currently running yt-dlp, terminate it so we
        // don't stall the dtor for up to 15 s.
        if (current_child_) {
            TerminateProcess((HANDLE)current_child_, 1);
        }
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

std::string YoutubeResolver::locateYtDlp() {
    // 1) Portable drop next to Vela.exe wins - it's what the user
    //    consciously placed there.
    std::string portable = appDir() + "\\yt-dlp.exe";
    DWORD attrs = GetFileAttributesA(portable.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return portable;
    }

    // 2) PATH lookup via where.exe. Capture stdout through an anonymous
    //    pipe; the first line is the absolute path we want.
    HANDLE read_h  = nullptr;
    HANDLE write_h = nullptr;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&read_h, &write_h, &sa, 0)) return {};
    // The read end must NOT be inherited - otherwise the child could hold
    // it open and ReadFile would block after the child exits.
    SetHandleInformation(read_h, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = write_h;
    si.hStdError  = write_h;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    char cmd[] = "where.exe yt-dlp.exe";
    BOOL ok = CreateProcessA(nullptr, cmd, nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(write_h);
    if (!ok) {
        CloseHandle(read_h);
        return {};
    }
    // Bounded wait - where.exe is essentially instant, but a hung shell
    // hook could stall us; cap at 3 s.
    WaitForSingleObject(pi.hProcess, 3000);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    std::string out = readAllFromPipe(read_h);
    CloseHandle(read_h);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (exit_code != 0) return {};

    std::string first = out;
    size_t nl = first.find_first_of("\r\n");
    if (nl != std::string::npos) first = first.substr(0, nl);
    first = trimWs(first);
    return first;
}

uint64_t YoutubeResolver::requestResolve(const std::string& url) {
    uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        id = next_id_++;
        // Any in-flight yt-dlp for an earlier request is superseded; kill
        // it so the worker isn't blocked on WaitForSingleObject for the
        // stale one. The child's read pipe will EOF, runYtDlp returns
        // failure, and the worker compares latest_request_id_ before
        // enqueueing - so nothing surfaces from the old work.
        latest_request_id_ = id;
        if (current_child_) {
            TerminateProcess((HANDLE)current_child_, 1);
        }
        pending_id_  = id;
        pending_url_ = url;
        has_pending_ = true;

        // yt-dlp missing: enqueue the failure result immediately from
        // the caller's thread. Cheaper than a round trip through the
        // worker and lets the OSD fire on the next frame. We can ONLY
        // do this once the initial probe has finished - before that
        // yt_dlp_available_ is default-false and would falsely fail
        // legitimate requests. During the probe window (~500 ms after
        // startup) the worker handles the request after it finishes
        // probing, which is exactly the ordering we want.
        if (probe_done_.load(std::memory_order_acquire)
            && !yt_dlp_available_) {
            Result r;
            r.id         = id;
            r.source_url = url;
            r.ok         = false;
            r.message    = "yt-dlp not installed";
            results_.push_back(std::move(r));
            has_pending_ = false;
        }
    }
    cv_.notify_all();
    return id;
}

bool YoutubeResolver::tryDrainResult(Result& out) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (results_.empty()) return false;
    out = std::move(results_.front());
    results_.pop_front();
    return true;
}

void YoutubeResolver::workerLoop() {
    // Phase 1 - probe. Runs off the main thread so the app startup path
    // (especially Explorer double-click of a video where UIController is
    // constructed before the first frame draws) doesn't stall on where.exe
    // + `yt-dlp --version`. Any resolve requests queued during this window
    // sit in has_pending_ until we finish probing, then get picked up
    // naturally by the request loop below.
    {
        std::string path = locateYtDlp();
        bool        avail = !path.empty();
        int         age = -1;
        std::string ver;
        if (avail) {
            age = fileAgeDays(path);
            ver = probeYtDlpVersion(path);
        }
        std::lock_guard<std::mutex> lk(mutex_);
        yt_dlp_path_      = std::move(path);
        yt_dlp_available_ = avail;
        yt_dlp_age_days_  = age;
        yt_dlp_version_   = std::move(ver);
        probe_done_.store(true, std::memory_order_release);
    }
    cv_.notify_all();   // wake anyone that Ctrl+V'd during the probe window

    // Phase 2 - normal request loop.
    for (;;) {
        uint64_t    id;
        std::string url;
        std::string yt_path;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [&]{ return quit_.load() || has_pending_; });
            if (quit_.load()) return;
            id           = pending_id_;
            url          = pending_url_;
            yt_path      = yt_dlp_path_;
            has_pending_ = false;
        }

        Result r;
        r.id         = id;
        r.source_url = url;
        if (yt_path.empty()) {
            r.ok      = false;
            r.message = "yt-dlp not installed";
        } else {
            std::string media;
            std::string err;
            r.ok = runYtDlp(yt_path, url, media, err);
            if (r.ok) r.media_url = media;
            else      r.message   = err.empty() ? std::string("resolve failed") : err;
        }

        // Only surface the result if it's still the newest request. If a
        // newer requestResolve came in mid-run we killed our child and
        // there's a fresh job waiting; the old result is discarded.
        std::lock_guard<std::mutex> lk(mutex_);
        if (id == latest_request_id_) {
            results_.push_back(std::move(r));
        }
    }
}

bool YoutubeResolver::runYtDlp(const std::string& yt_dlp_path,
                               const std::string& url,
                               std::string&       out_media_url,
                               std::string&       err_msg) {
    out_media_url.clear();
    err_msg.clear();

    // Separate pipes for stdout and stderr so we can grab the stderr tail
    // for user-facing error text without smashing it into the URL.
    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE out_r = nullptr, out_w = nullptr;
    HANDLE err_r = nullptr, err_w = nullptr;
    if (!CreatePipe(&out_r, &out_w, &sa, 0)) {
        err_msg = "pipe alloc failed";
        return false;
    }
    if (!CreatePipe(&err_r, &err_w, &sa, 0)) {
        CloseHandle(out_r); CloseHandle(out_w);
        err_msg = "pipe alloc failed";
        return false;
    }
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = out_w;
    si.hStdError  = err_w;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    // yt-dlp -g prints the direct URL(s) to stdout. --no-playlist keeps
    // us on the single video even when the URL has a list= parameter.
    // The format selector prefers a single-file mp4 so our existing
    // avformat_open_input pipeline stays happy (audio+video already
    // merged); falls back to whatever "best" is if mp4 isn't available.
    std::string cmd = "\"" + yt_dlp_path + "\""
        + " --no-warnings --no-playlist"
        + " -f \"best[ext=mp4]/best\""
        + " -g"
        + " \"" + url + "\"";

    // CreateProcessA mutates the command buffer - give it a writable copy.
    std::vector<char> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back('\0');

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(nullptr, cmd_buf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(out_w);
    CloseHandle(err_w);
    if (!ok) {
        CloseHandle(out_r);
        CloseHandle(err_r);
        err_msg = "failed to start yt-dlp";
        return false;
    }

    // Record the child handle so a superseding request can TerminateProcess
    // it. Also so the dtor doesn't get stuck waiting on a hung yt-dlp.
    {
        std::lock_guard<std::mutex> lk(mutex_);
        current_child_ = (void*)pi.hProcess;
    }

    // 15 s cap - yt-dlp normally resolves in 1-5 s. If we hit the timeout
    // or a cancellation, kill the process and drain the pipes.
    DWORD wr = WaitForSingleObject(pi.hProcess, 15000);
    bool  timed_out = (wr == WAIT_TIMEOUT);
    if (timed_out) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 1000);
    }

    std::string stdout_s = readAllFromPipe(out_r);
    std::string stderr_s = readAllFromPipe(err_r);
    CloseHandle(out_r);
    CloseHandle(err_r);

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    {
        std::lock_guard<std::mutex> lk(mutex_);
        current_child_ = nullptr;
    }

    if (timed_out) {
        err_msg = "timeout";
        return false;
    }
    if (exit_code != 0) {
        err_msg = tailError(stderr_s);
        if (err_msg.empty()) err_msg = "yt-dlp exit code non-zero";
        return false;
    }

    // yt-dlp -g with a merged-format selector prints a single URL line.
    // In the rare "video-only + audio-only" split case it prints two -
    // take the first, which is the video track; users will hear no audio
    // but at least see the picture (edge case, unlikely with our format
    // string).
    std::string first = stdout_s;
    size_t nl = first.find_first_of("\r\n");
    if (nl != std::string::npos) first = first.substr(0, nl);
    first = trimWs(first);

    if (first.empty()) {
        err_msg = "yt-dlp produced no URL";
        return false;
    }
    // Sanity: it should look like a URL (http/https). yt-dlp occasionally
    // emits diagnostic lines to stdout too; if the first line isn't a URL
    // we treat it as failure.
    if (!(startsWithI(first, "http://") || startsWithI(first, "https://"))) {
        err_msg = tailError(stderr_s);
        if (err_msg.empty()) err_msg = "unexpected yt-dlp output";
        return false;
    }

    out_media_url = first;
    return true;
}
