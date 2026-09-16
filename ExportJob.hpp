#pragma once
#include <atomic>
#include <string>
#include <thread>
#include <SDL3/SDL.h>

struct ExportControl {
    std::atomic<bool> cancelled{false};
    std::atomic<float> progress{0.0f};
    uint64_t deadline = 0; // Only used on the export worker.
    void touch() { deadline = SDL_GetTicks() + 5000; }
    static int interrupt(void* opaque) {
        auto* self = static_cast<ExportControl*>(opaque);
        return self->cancelled.load() || (self->deadline && SDL_GetTicks() >= self->deadline);
    }
};

class ExportJob {
public:
    enum class State { Idle, Running, Succeeded, Failed, Cancelled };
    ~ExportJob();
    bool start(const std::string& source, const std::string& destination,
               double begin, double end, bool gif);
    void cancel() { control_.cancelled.store(true); }
    bool running() const { return state_.load() == State::Running; }
    float progress() const { return control_.progress.load(); }
    State takeResult();
private:
    ExportControl control_;
    std::atomic<State> state_{State::Idle};
    std::thread worker_;
};
