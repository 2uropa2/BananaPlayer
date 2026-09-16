#include "ExportJob.hpp"
#include "VideoPlayer.hpp"

ExportJob::~ExportJob() {
    cancel();
    if (worker_.joinable()) worker_.join();
}
bool ExportJob::start(const std::string& source, const std::string& destination,
                      double begin, double end, bool gif) {
    if (running()) return false;
    if (worker_.joinable()) worker_.join();
    control_.cancelled.store(false);
    control_.progress.store(0.0f);
    state_.store(State::Running);
    try {
        worker_ = std::thread([this, source, destination, begin, end, gif] {
            bool ok = false;
            try {
                ok = gif ? VideoPlayer::exportSegmentAsGif(source, destination, begin, end, 15, 480, &control_)
                         : VideoPlayer::exportSegment(source, destination, begin, end, &control_);
            } catch (...) { ok = false; }
            // A commit that completed before Cancel was pressed is still success.
            state_.store(ok ? State::Succeeded : control_.cancelled.load() ? State::Cancelled : State::Failed);
        });
    } catch (...) {
        state_.store(State::Failed);
        return false;
    }
    return true;
}
ExportJob::State ExportJob::takeResult() {
    const State state = state_.load();
    if (state == State::Idle || state == State::Running) return State::Idle;
    if (worker_.joinable()) worker_.join();
    state_.store(State::Idle);
    return state;
}
