#include "VideoPlayer.hpp"
#include "Thumbnail.hpp"
#include "Subtitles.hpp"
#include "License.hpp"
#include "Promo.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;
static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static std::string read(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
struct Display {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    Display() {
        require(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO), SDL_GetError());
        window = SDL_CreateWindow("Banana integration", 320, 180, SDL_WINDOW_HIDDEN);
        require(window != nullptr, SDL_GetError());
        renderer = SDL_CreateRenderer(window, "software");
        require(renderer != nullptr, SDL_GetError());
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV,
                                    SDL_TEXTUREACCESS_STREAMING, 320, 180);
        require(texture != nullptr, SDL_GetError());
    }
    ~Display() {
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
    }
};
static void pump(VideoPlayer& player, SDL_Texture* texture, int milliseconds) {
    auto deadline = SDL_GetTicks() + milliseconds;
    while (SDL_GetTicks() < deadline) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {}
        player.updateTexture(texture);
        SDL_Delay(5);
    }
}
int main(int argc, char** argv) {
    try {
        require(argc >= 3, "usage: banana_integration CASE FIXTURE_OR_URL");
        const std::string mode = argv[1];
        const std::string input = argv[2];
        if (mode == "promo-snapshot") {
            struct Shutdown { ~Shutdown() { Promo::shutdown(); } } shutdown;
            const auto directory = fs::u8path(input).parent_path();
            { std::ofstream f(directory / "banana.promo.json"); f << R"({"headline":"old banner"})"; }
            Promo::start(directory.u8string());
            const auto start = SDL_GetTicks();
            while (!Promo::current() && SDL_GetTicks() - start < 2000) SDL_Delay(5);
            const auto captured = Promo::current();
            require(captured && captured->headline == "old banner", "Cached banner did not load");
            { std::ofstream f(directory / "promo-ready"); f << "ready"; }
            bool updated = false;
            while (SDL_GetTicks() - start < 4000) {
                auto latest = Promo::current();
                if (latest && latest->headline == "new banner") { updated = true; break; }
                SDL_Delay(5);
            }
            require(updated, "Local promo update did not arrive");
            require(captured->headline == "old banner", "Previously returned banner changed after background update");
        } else if (mode == "license-legacy" || mode == "license-v5") {
            const auto directory = fs::u8path(input).parent_path() / "license-state";
            fs::create_directories(directory);
            License::init(directory.u8string());
            const auto fixtures = fs::path(BP_TEST_FIXTURES);
            if (mode == "license-legacy") {
                require(!License::activate("test@example.invalid", read(fixtures / "license_legacy.txt")), "Insecure V4 key accepted");
            } else {
                const auto key = read(fixtures / "license_valid.txt");
                require(License::activate(" TEST@example.invalid ", key), "Signed license did not verify");
                require(License::isPro(), "Valid license did not activate Pro");
                License::init(directory.u8string());
                require(License::isPro(), "Saved license did not survive reload");
                require(!License::activate("another@example.invalid", key), "Signature accepted for a different email");
                std::string tampered = key;
                tampered[20] = tampered[20] == 'A' ? 'B' : 'A';
                require(!License::activate("test@example.invalid", tampered), "Tampered signature accepted");
                require(!License::isPro(), "Failed verification retained Pro state");
            }
            License::deactivate();
        } else if (mode == "state-utf8") {
            const auto directory = fs::u8path(input).parent_path() / fs::u8path(u8"설정_日本語_😀");
            fs::create_directories(directory);
            const auto path = directory / "banana.license";
            { std::ofstream f(path); f << "test@example.invalid\ninvalid-key\n"; }
            License::init(directory.u8string());
            require(License::status() == License::Status::Invalid, "Unicode state file was not read");
            License::deactivate();
            require(!fs::exists(path), "Unicode state file was not removed");
        } else if (mode == "snapshot-utf8") {
            const auto path = fs::u8path(input).parent_path() / fs::u8path(u8"한글_日本語_😀.png");
            std::vector<uint8_t> pixels(32 * 32 * 4, 180);
            require(VideoPlayer::snapshotFromBytes(path.u8string(), 32, 32,
                    AV_PIX_FMT_RGBA, 32 * 4, pixels.data()), "Unicode snapshot failed");
            require(read(path).substr(0, 8) == std::string("\x89PNG\r\n\x1a\n", 8), "Invalid PNG output");
        } else if (mode == "export-original") {
            const auto source = fs::u8path(input);
            const auto before = read(source);
            const bool result = VideoPlayer::exportSegment(input, input, 0.0, 1.0);
            require(read(source) == before, "Export modified its own source file");
            require(!result, "Export accepted its own source as destination");
        } else if (mode == "export-failure") {
            const auto destination = fs::u8path(input).parent_path() / "existing.wav";
            { std::ofstream f(destination, std::ios::binary); f << "keep existing destination"; }
            const auto before = read(destination);
            require(!VideoPlayer::exportSegment(input, destination.u8string(), 0.0, 1.0), "Invalid container accepted video");
            require(read(destination) == before, "Failed export damaged the existing destination");
        } else if (mode == "export-copy") {
            const auto destination = fs::u8path(input).parent_path() / fs::u8path(u8"내보내기_日本語.mkv");
            require(VideoPlayer::exportSegment(input, destination.u8string(), 0.0, 1.0), "Segment export failed");
            AVFormatContext* context = nullptr;
            require(avformat_open_input(&context, destination.u8string().c_str(), nullptr, nullptr) == 0, "Export could not reopen");
            require(avformat_find_stream_info(context, nullptr) >= 0, "Export has no readable streams");
            require(context->nb_streams >= 2, "Export lost audio or video");
            avformat_close_input(&context);
        } else if (mode == "export-alias") {
            const auto source = fs::u8path(input);
            const auto alias = source.parent_path() / "source-hardlink.mkv";
            fs::create_hard_link(source, alias);
            const auto before = read(source);
            require(!VideoPlayer::exportSegment(input, alias.u8string(), 0, 1), "Export accepted a source hard link");
            require(read(source) == before, "Export changed original through hard link");
#ifdef BP_HAS_EXPORT_JOB
        } else if (mode == "export-cancel") {
            const auto destination = fs::current_path() / "cancelled.mkv";
            { std::ofstream f(destination); f << "original destination"; }
            const auto before = read(destination);
            ExportJob job;
            const auto start = SDL_GetTicks();
            require(job.start(input, destination.u8string(), 0, 1, false), "Worker did not start");
            require(SDL_GetTicks() - start < 200, "Starting export blocked caller");
            SDL_Delay(300);
            job.cancel();
            while (job.running() && SDL_GetTicks() - start < 2000) SDL_Delay(10);
            require(job.takeResult() == ExportJob::State::Cancelled, "Export cancellation did not finish");
            require(read(destination) == before, "Cancelled export changed destination");
            for (const auto& entry : fs::directory_iterator(fs::current_path()))
                require(entry.path().filename().u8string().find(".banana-") == std::string::npos, "Cancelled export left a temporary file");
#endif
        } else if (mode == "thumbnail-close") {
            Display display;
            auto start = SDL_GetTicks();
            {
                ThumbnailProvider thumbnails(display.renderer);
                thumbnails.openFile(input);
                SDL_Delay(300);
            }
            require(SDL_GetTicks() - start < 2000, "Thumbnail shutdown exceeded two seconds");
        } else if (mode == "network-close") {
            Display display;
            VideoPlayer player;
            require(player.loadFile(input), "Network fixture did not finish probing");
            std::cout << "opened network fixture" << std::endl;
            player.play();
            pump(player, display.texture, 1200);
            const auto start = SDL_GetTicks();
            player.unload();
            require(SDL_GetTicks() - start < 2000, "Network unload exceeded two seconds");
        } else if (mode == "playback") {
            Display display;
            VideoPlayer player;
            require(player.loadFile(input), "Fixture could not load");
            require(player.hasVideo() && player.hasAudio(), "Fixture needs audio and video");
            player.play();
            pump(player, display.texture, 700);
            require(player.getFramesDisplayed() >= 5, "Video frames did not advance");
            require(player.getCurrentTime() > 0.2, "Playback clock did not advance");
            player.pause();
            const auto paused = player.getCurrentTime();
            pump(player, display.texture, 150);
            require(std::abs(player.getCurrentTime() - paused) < 0.06, "Clock advanced while paused");
            player.seek(1.0);
            player.play();
            pump(player, display.texture, 450);
            require(player.getCurrentTime() >= 0.9, "Seek did not reach target");
            player.unload();
            require(!player.isLoaded(), "Unload did not reset player");
        } else {
            throw std::runtime_error("Unknown integration case");
        }
        std::cout << "PASS " << mode << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << std::endl;
        return 1;
    }
}
