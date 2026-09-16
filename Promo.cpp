#include "FileIO.hpp"
#include "Promo.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

namespace Promo {

namespace {

// Vendor endpoint. Keep as a compile-time constant so we don't ship
// an unprotected knob. If the domain changes, bump the build.
constexpr const char* kManifestUrl =
    "https://banana-player.com/promo.json";

// Ceiling on how much manifest body we're willing to accept. A legit
// text-only banner is well under 4 KB; anything larger is either a
// misconfigured endpoint or malicious. Reject beyond this.
constexpr size_t kMaxBodyBytes = 16 * 1024;

std::mutex           g_mutex;
Banner               g_current;
bool                 g_have_current  = false;
std::atomic<bool>    g_suppressed{false};
std::atomic<bool>    g_running{false};
std::atomic<bool>    g_quit{false};
std::thread          g_worker;
std::string          g_cache_path;

// -------- Tiny JSON extractor (single-object, string-value only) --------
//
// The manifest is a flat object with string values only, no arrays and
// no nesting - we don't want to pull in a full JSON dependency for one
// endpoint. This parser recognises exactly the fields we need. Anything
// unexpected: silently ignored.

std::string extractField(const std::string& body, const std::string& key) {
    // Find "key" pattern.
    std::string needle = "\"" + key + "\"";
    size_t p = body.find(needle);
    if (p == std::string::npos) return {};
    p += needle.size();
    // Skip whitespace and colon.
    while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) ++p;
    if (p >= body.size() || body[p] != ':') return {};
    ++p;
    while (p < body.size() && (body[p] == ' ' || body[p] == '\t' || body[p] == '\n' || body[p] == '\r')) ++p;
    // Value must be a string.
    if (p >= body.size() || body[p] != '"') return {};
    ++p;
    std::string out;
    while (p < body.size() && body[p] != '"') {
        if (body[p] == '\\' && p + 1 < body.size()) {
            char n = body[p + 1];
            switch (n) {
                case 'n':  out.push_back('\n'); p += 2; continue;
                case 't':  out.push_back('\t'); p += 2; continue;
                case '"':  out.push_back('"');  p += 2; continue;
                case '\\': out.push_back('\\'); p += 2; continue;
                case '/':  out.push_back('/');  p += 2; continue;
                default:   out.push_back(n);    p += 2; continue;
            }
        }
        out.push_back(body[p++]);
    }
    return out;
}

bool parseBanner(const std::string& body, Banner& out) {
    Banner b;
    b.headline   = extractField(body, "headline");
    b.subline    = extractField(body, "subline");
    b.click_url  = extractField(body, "click_url");
    b.expires_at = extractField(body, "expires_at");
    if (b.headline.empty()) return false;
    out = std::move(b);
    return true;
}

bool isExpired(const std::string& iso_date) {
    // Empty = never expires.
    if (iso_date.empty()) return false;
    int y = 0, m = 0, d = 0;
    if (std::sscanf(iso_date.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return false;
    std::time_t now = std::time(nullptr);
    std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &now);
#else
    lt = *std::localtime(&now);
#endif
    int today_y = lt.tm_year + 1900;
    int today_m = lt.tm_mon + 1;
    int today_d = lt.tm_mday;
    if (y != today_y) return y < today_y;
    if (m != today_m) return m < today_m;
    return d < today_d;
}

// -------- Cache I/O --------

bool loadCache(Banner& out) {
    std::FILE* f = FileIO::open(g_cache_path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (size_t)sz > kMaxBodyBytes) { std::fclose(f); return false; }
    std::string body((size_t)sz, '\0');
    size_t got = std::fread(body.data(), 1, (size_t)sz, f);
    std::fclose(f);
    if (got != (size_t)sz) return false;
    Banner b;
    if (!parseBanner(body, b)) return false;
    if (isExpired(b.expires_at)) return false;
    out = std::move(b);
    return true;
}

void saveCache(const std::string& body) {
    std::string tmp = g_cache_path + ".tmp";
    std::FILE* f = FileIO::open(tmp.c_str(), "wb");
    if (!f) return;
    std::fwrite(body.data(), 1, body.size(), f);
    std::fflush(f);
    std::fclose(f);
#ifdef _WIN32
    FileIO::replace(tmp, g_cache_path);
#else
    std::rename(tmp.c_str(), g_cache_path.c_str());
#endif
}

// -------- Fetch --------
//
// WinHTTP-based HTTPS GET. Returns body (or empty on any error) with
// a 5-second connect / 5-second receive timeout so a dead endpoint
// can't tie up the worker thread past that.

#ifdef _WIN32
std::string httpsGet(const std::string& url) {
    std::wstring wurl(url.size(), L'\0');
    int wlen = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), (int)url.size(),
                                    wurl.data(), (int)wurl.size());
    if (wlen <= 0) return {};
    wurl.resize((size_t)wlen);

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0};
    wchar_t path[1024] = {0};
    uc.lpszHostName     = host; uc.dwHostNameLength     = 255;
    uc.lpszUrlPath      = path; uc.dwUrlPathLength      = 1023;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return {};

    HINTERNET session = WinHttpOpen(L"BananaPlayer/1.4 promo fetcher",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return {};
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);
    HINTERNET connect = WinHttpConnect(session, host, uc.nPort, 0);
    if (!connect) { WinHttpCloseHandle(session); return {}; }
    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path,
                                           nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return {};
    }
    std::string body;
    if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        && WinHttpReceiveResponse(request, nullptr)) {
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(request, &avail) && avail > 0) {
            if (body.size() + avail > kMaxBodyBytes) break;
            std::vector<char> chunk(avail);
            DWORD got = 0;
            if (!WinHttpReadData(request, chunk.data(), avail, &got) || got == 0) break;
            body.append(chunk.data(), got);
        }
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return body;
}
#else
std::string httpsGet(const std::string&) { return {}; }
#endif

void workerLoop() {
    // Load stale cache first so current() has an answer immediately.
    {
        Banner cached;
        if (loadCache(cached)) {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_current = std::move(cached);
            g_have_current = true;
        }
    }
    // Fetch fresh. Silent on failure - the cache (or nothing) stands.
    if (g_quit.load()) return;
    std::string body = httpsGet(kManifestUrl);
    if (body.empty()) return;
    Banner fresh;
    if (!parseBanner(body, fresh)) return;
    if (isExpired(fresh.expires_at)) return;
    saveCache(body);
    std::lock_guard<std::mutex> lk(g_mutex);
    g_current = std::move(fresh);
    g_have_current = true;
}

}   // namespace

void start(const std::string& cache_dir) {
    if (g_running.exchange(true)) return;
    g_quit.store(false);
    // Trim trailing slash then join.
    std::string dir = cache_dir;
    while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
    g_cache_path = dir + "\\banana.promo.json";
    g_worker = std::thread(workerLoop);
}

void shutdown() {
    g_quit.store(true);
    if (g_worker.joinable()) g_worker.join();
    g_running.store(false);
    std::lock_guard<std::mutex> lk(g_mutex);
    g_have_current = false;
}

const Banner* current() {
    if (g_suppressed.load()) return nullptr;
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_have_current) return nullptr;
    // Re-check expiry in case a very stale cache slid past the loader
    // check (e.g. system clock changed mid-session).
    if (isExpired(g_current.expires_at)) return nullptr;
    return &g_current;
}

void setSuppressed(bool suppressed) {
    g_suppressed.store(suppressed);
}

}   // namespace Promo
