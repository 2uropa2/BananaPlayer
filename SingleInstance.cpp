#include "SingleInstance.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace SingleInstance {

namespace {

#ifdef _WIN32
const char* kMutexName = "Global\\BananaPlayer.SingleInstance";
const char* kPipeName  = "\\\\.\\pipe\\BananaPlayer.Cmd";

// State guarded by g_mutex. All state modification lives here so the UI
// thread (activate/deactivate from menu) doesn't race the pipe worker.
std::mutex               g_state_mutex;
HANDLE                   g_instance_mutex   = nullptr;
bool                     g_another_running  = false;
std::thread              g_pipe_thread;
std::atomic<bool>        g_pipe_thread_quit{false};
Uint32                   g_load_event       = (Uint32)-1;

void pipeListenerLoop() {
    while (!g_pipe_thread_quit.load()) {
        HANDLE hp = CreateNamedPipeA(kPipeName,
            PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            0, 4096, 500, nullptr);
        if (hp == INVALID_HANDLE_VALUE) {
            Sleep(200); continue;
        }
        OVERLAPPED ov{};
        HANDLE hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        ov.hEvent = hEvent;
        BOOL connected = ConnectNamedPipe(hp, &ov);
        if (!connected && GetLastError() == ERROR_IO_PENDING) {
            while (!g_pipe_thread_quit.load()) {
                DWORD w = WaitForSingleObject(hEvent, 200);
                if (w == WAIT_OBJECT_0) { connected = TRUE; break; }
                if (w != WAIT_TIMEOUT)  { break; }
            }
        } else if (!connected && GetLastError() == ERROR_PIPE_CONNECTED) {
            connected = TRUE;
        }
        if (!connected || g_pipe_thread_quit.load()) {
            CancelIoEx(hp, &ov);
            CloseHandle(hEvent);
            DisconnectNamedPipe(hp);
            CloseHandle(hp);
            if (g_pipe_thread_quit.load()) break;
            continue;
        }
        CloseHandle(hEvent);

        char buf[4096];
        DWORD n_read = 0;
        BOOL rok = ReadFile(hp, buf, sizeof(buf) - 1, &n_read, nullptr);
        if (rok && n_read > 0) {
            buf[n_read] = '\0';
            while (n_read > 0 &&
                   (buf[n_read - 1] == '\r' || buf[n_read - 1] == '\n'
                    || buf[n_read - 1] == ' ' || buf[n_read - 1] == '\t')) {
                buf[--n_read] = '\0';
            }
            if (n_read > 0 && g_load_event != (Uint32)-1) {
                SDL_Event ev;
                SDL_zero(ev);
                ev.type = g_load_event;
                ev.user.data1 = new std::string(buf);   // main-thread frees
                SDL_PushEvent(&ev);
            }
        }
        DisconnectNamedPipe(hp);
        CloseHandle(hp);
    }
}
#endif   // _WIN32

}   // anonymous namespace

Uint32 registerSdlEvent() {
#ifdef _WIN32
    if (g_load_event == (Uint32)-1) {
        g_load_event = SDL_RegisterEvents(1);
    }
    return g_load_event;
#else
    return (Uint32)-1;
#endif
}

Uint32 sdlEventType() {
#ifdef _WIN32
    return g_load_event;
#else
    return (Uint32)-1;
#endif
}

bool isPrimary() {
#ifdef _WIN32
    std::lock_guard<std::mutex> lk(g_state_mutex);
    return g_instance_mutex != nullptr;
#else
    return false;
#endif
}

bool anotherInstanceRunning() {
#ifdef _WIN32
    std::lock_guard<std::mutex> lk(g_state_mutex);
    return g_another_running;
#else
    return false;
#endif
}

bool activate() {
#ifdef _WIN32
    // Fast path: already primary.
    {
        std::lock_guard<std::mutex> lk(g_state_mutex);
        if (g_instance_mutex) return true;
    }
    HANDLE h = CreateMutexA(nullptr, TRUE, kMutexName);
    DWORD err = GetLastError();
    if (!h) return false;
    if (err == ERROR_ALREADY_EXISTS) {
        CloseHandle(h);
        std::lock_guard<std::mutex> lk(g_state_mutex);
        g_another_running = true;
        return false;
    }
    // We're primary now. Publish the mutex + spin the listener.
    {
        std::lock_guard<std::mutex> lk(g_state_mutex);
        g_instance_mutex  = h;
        g_another_running = false;
        g_pipe_thread_quit.store(false);
    }
    // Registration is idempotent - safe even if main.cpp already did it.
    registerSdlEvent();
    // Start the listener OUTSIDE the state mutex so the thread's own
    // work (CreateNamedPipeA + wait) doesn't stall the state lock.
    std::thread t(pipeListenerLoop);
    {
        std::lock_guard<std::mutex> lk(g_state_mutex);
        g_pipe_thread = std::move(t);
    }
    return true;
#else
    return false;
#endif
}

void deactivate() {
#ifdef _WIN32
    std::thread grabbed;
    HANDLE grabbed_mutex = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_state_mutex);
        if (g_pipe_thread.joinable()) {
            g_pipe_thread_quit.store(true);
            grabbed = std::move(g_pipe_thread);
        }
        grabbed_mutex = g_instance_mutex;
        g_instance_mutex = nullptr;
        g_another_running = false;
    }
    if (grabbed.joinable()) {
        // Poke the listener out of its overlapped wait by connecting
        // once ourselves. If the CreateFileA fails that's fine - the
        // 200 ms poll will pick up g_pipe_thread_quit within a beat.
        HANDLE poke = CreateFileA(kPipeName, GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
        if (poke != INVALID_HANDLE_VALUE) CloseHandle(poke);
        grabbed.join();
    }
    if (grabbed_mutex) {
        ReleaseMutex(grabbed_mutex);
        CloseHandle(grabbed_mutex);
    }
#endif
}

bool peekPolicyFromConfig() {
#ifdef _WIN32
    std::vector<std::string> candidates;
    if (const char* appdata = std::getenv("APPDATA")) {
        std::string p(appdata);
        while (!p.empty() && (p.back() == '\\' || p.back() == '/')) p.pop_back();
        candidates.push_back(p + "\\BananaPlayer\\banana.config");
        // Legacy fallback so upgrades honor the old-installed policy
        // until the migration writes the new file.
        candidates.push_back(p + "\\Vela\\vela.config");
    }
    {
        char exe[MAX_PATH];
        DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            std::string s(exe, n);
            size_t p = s.find_last_of("\\/");
            candidates.push_back((p == std::string::npos)
                                    ? std::string("banana.config")
                                    : s.substr(0, p) + "\\banana.config");
        }
    }
    for (const std::string& cfg : candidates) {
        FILE* fp = nullptr;
        if (fopen_s(&fp, cfg.c_str(), "rb") != 0 || !fp) continue;
        char line[256];
        bool found_key = false;
        bool result = false;
        while (std::fgets(line, sizeof(line), fp)) {
            const char* k = line;
            while (*k == ' ' || *k == '\t') ++k;
            if (std::strncmp(k, "single_instance", 15) != 0) continue;
            const char* eq = std::strchr(k, '=');
            if (!eq) continue;
            ++eq;
            while (*eq == ' ' || *eq == '\t') ++eq;
            found_key = true;
            if (*eq == '1') { result = true; break; }
        }
        std::fclose(fp);
        if (found_key) return result;
    }
    return false;
#else
    return false;
#endif
}

bool forwardPathToPrimary(const std::string& path) {
#ifdef _WIN32
    for (int attempt = 0; attempt < 20; ++attempt) {
        HANDLE hp = CreateFileA(kPipeName, GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, 0, nullptr);
        if (hp != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            BOOL ok = WriteFile(hp, path.data(), (DWORD)path.size(),
                                &written, nullptr);
            FlushFileBuffers(hp);
            CloseHandle(hp);
            return ok && written == (DWORD)path.size();
        }
        DWORD err = GetLastError();
        if (err != ERROR_PIPE_BUSY) break;
        WaitNamedPipeA(kPipeName, 100);
    }
    return false;
#else
    (void)path;
    return false;
#endif
}

}   // namespace SingleInstance
