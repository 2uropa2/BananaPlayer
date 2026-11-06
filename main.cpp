#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "VideoPlayer.hpp"
#include "License.hpp"
#include "UIController.hpp"
#include "Style.hpp"
#include "SingleInstance.hpp"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    // Must match IDI_APPICON in resources/app.rc.
    #define IDI_APPICON 101
#endif

// Single-instance handoff lives in SingleInstance.{hpp,cpp}. main.cpp
// only decides policy at startup + wires the load-path SDL user event
// through to ui.openAny; the UIController Settings toggle can also
// activate/deactivate mid-session by calling into the same module.

// ---------- One-frame render, callable during the modal move/size loop ----------
//
// When Windows enters a modal loop (title-bar drag, edge resize), the
// caller's SDL_PollEvent thread is blocked inside user32's own loop
// and never returns. Any WM_TIMER we set up on the window still gets
// dispatched to the WndProc though, so we hook that: WM_ENTERSIZEMOVE
// installs a timer, WM_TIMER calls renderOneFrame() to keep the
// picture live, WM_EXITSIZEMOVE tears the timer down. The shared
// state (renderer, player, ui, video_texture, ...) lives in file-
// scope statics so both the normal main-loop caller and the modal
// hook can drive rendering without either owning a copy.
namespace {

struct AppRenderCtx {
    SDL_Window*    window          = nullptr;
    SDL_Renderer*  renderer        = nullptr;
    VideoPlayer*   player          = nullptr;
    UIController*  ui              = nullptr;
    SDL_Texture*   video_texture   = nullptr;
    int            tex_w           = 0;
    int            tex_h           = 0;
    // Snapshot of ui->getTextureGeneration() at the moment
    // video_texture was created. When the UI bumps its counter (e.g.
    // Studio-range toggle in Settings), we rebuild the texture even
    // though dims are unchanged. Sentinel ~0u forces first-time build.
    unsigned int   tex_generation  = ~0u;
    // Last SDL_Colorspace the texture was created with. Tracked
    // separately from tex_generation because the colorspace is now
    // resolved from the stream's codecpar - switching to a differently
    // tagged file at the same resolution must still trigger a rebuild.
    // SDL_COLORSPACE_UNKNOWN = 0 is the "not yet set" sentinel.
    SDL_Colorspace tex_colorspace   = SDL_COLORSPACE_UNKNOWN;
    bool           has_first_frame = false;
};
AppRenderCtx g_app;

void renderOneFrame() {
    if (!g_app.renderer || !g_app.ui || !g_app.player || !g_app.window) return;
    VideoPlayer& player  = *g_app.player;
    UIController& ui     = *g_app.ui;
    SDL_Window* window   = g_app.window;
    SDL_Renderer* renderer = g_app.renderer;

    // Ensure texture matches the output dims (not necessarily source
    // dims - HDR half-res mode ships a downscaled tone-map). SDL
    // upscales during SDL_RenderTexture so the aspect stays right.
    // Audio-only files have no video stream: skip texture management
    // entirely so the placeholder in UIController::drawAudioOnlyPlaceholder
    // fills the video area instead of a stale prior-file texture.
    if (player.isLoaded() && !player.isAudioOnly()) {
        int vw = player.getOutputWidth();
        int vh = player.getOutputHeight();
        unsigned int ui_gen = ui.getTextureGeneration();

        // Resolve SDL colorspace from the stream's codecpar + user
        // override up front so we can gate on it. Full-range is the
        // exception (MJPEG / JPEG stills / some phone camera streams
        // tagged AVCOL_RANGE_JPEG); Limited is the right default for
        // WEB-DL / Blu-ray / broadcast and anything with UNSPECIFIED
        // range. BT.601 vs BT.709 follows the stream matrix tag with
        // a resolution fallback (SD < 720 picks 601, HD/UHD picks 709)
        // - most modern content is 709.
        bool full_range;
        {
            UIController::YuvRangeMode mode = ui.getYuvRangeMode();
            if (mode == UIController::YuvRangeMode::ForceLimited)      full_range = false;
            else if (mode == UIController::YuvRangeMode::ForceFull)    full_range = true;
            else /* Auto */ full_range =
                (player.getVideoColorRange() == AVCOL_RANGE_JPEG);
        }
        bool bt601;
        {
            AVColorSpace cs_tag = player.getVideoColorSpace();
            switch (cs_tag) {
                case AVCOL_SPC_BT709:
                    bt601 = false; break;
                case AVCOL_SPC_SMPTE170M:
                case AVCOL_SPC_BT470BG:
                case AVCOL_SPC_FCC:
                case AVCOL_SPC_SMPTE240M:
                    bt601 = true;  break;
                default:
                    // Unspecified (or BT.2020 / other post-tone-map) -
                    // pick by resolution. HDR tone-map output lands in
                    // BT.709 regardless so that path falls here too.
                    bt601 = (vh > 0 && vh < 720);
                    break;
            }
        }
        SDL_Colorspace cs;
        if (bt601)   cs = full_range ? SDL_COLORSPACE_BT601_FULL  : SDL_COLORSPACE_BT601_LIMITED;
        else         cs = full_range ? SDL_COLORSPACE_BT709_FULL  : SDL_COLORSPACE_BT709_LIMITED;

        if (vw > 0 && vh > 0
            && (vw != g_app.tex_w || vh != g_app.tex_h
                || !g_app.video_texture
                || ui_gen != g_app.tex_generation
                || cs != g_app.tex_colorspace)) {
            if (g_app.video_texture) SDL_DestroyTexture(g_app.video_texture);
            SDL_PropertiesID props = SDL_CreateProperties();
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,
                                  SDL_PIXELFORMAT_IYUV);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,
                                  SDL_TEXTUREACCESS_STREAMING);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, vw);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, vh);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER,
                                  (Sint64)cs);
            g_app.video_texture = SDL_CreateTextureWithProperties(renderer, props);
            SDL_DestroyProperties(props);
            if (g_app.video_texture)
                SDL_SetTextureScaleMode(g_app.video_texture, SDL_SCALEMODE_LINEAR);
            g_app.tex_w = vw; g_app.tex_h = vh;
            g_app.tex_generation = ui_gen;
            g_app.tex_colorspace = cs;
            g_app.has_first_frame = false;
        }
    } else if (player.isLoaded() && player.isAudioOnly() && g_app.video_texture) {
        // Switched from a video file to an audio-only file: drop the
        // previous file's texture so its last frame doesn't linger under
        // the audio-only placeholder.
        SDL_DestroyTexture(g_app.video_texture);
        g_app.video_texture   = nullptr;
        g_app.tex_w           = 0;
        g_app.tex_h           = 0;
        g_app.tex_colorspace  = SDL_COLORSPACE_UNKNOWN;
        g_app.has_first_frame = false;
    }
    if (g_app.video_texture && !player.isAudioOnly()) {
        if (player.updateTexture(g_app.video_texture)) g_app.has_first_frame = true;
    }

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    int ww = 0, wh = 0;
    SDL_GetWindowSize(window, &ww, &wh);

    if (g_app.video_texture && g_app.tex_w > 0 && g_app.tex_h > 0
        && g_app.has_first_frame) {
        // src_aspect includes the video stream's SAR (pixel aspect). For
        // square pixels SAR is 1:1 and this reduces to width/height. For
        // anamorphic sources (e.g. shortform re-encodes coded 1920x1080 but
        // tagged 607:1920 to display as portrait) the SAR shrinks or
        // stretches the effective width so we letterbox to the intended
        // display aspect instead of the coded 16:9.
        AVRational sar = player.getVideoSampleAspectRatio();
        if (sar.num <= 0 || sar.den <= 0) { sar.num = 1; sar.den = 1; }
        float src_aspect = ((float)g_app.tex_w * (float)sar.num)
                         / ((float)g_app.tex_h * (float)sar.den);
        int intrinsic_deg = player.getIntrinsicRotation();
        int user_deg = 0;
        switch (ui.getRotation()) {
            case RotationMode::Deg90:  user_deg =  90; break;
            case RotationMode::Deg180: user_deg = 180; break;
            case RotationMode::Deg270: user_deg = 270; break;
            default:                                    break;
        }
        int total_deg = ((intrinsic_deg + user_deg) % 360 + 360) % 360;
        bool rot90 = (total_deg == 90 || total_deg == 270);
        double angle = (double)total_deg;

        float eff_aspect = rot90 ? (1.0f / src_aspect) : src_aspect;
        float dst_aspect = (float)ww / (float)wh;
        float box_w, box_h;
        if (dst_aspect > eff_aspect) {
            box_h = (float)wh; box_w = wh * eff_aspect;
        } else {
            box_w = (float)ww; box_h = ww / eff_aspect;
        }
        float zoom = ui.getVideoZoom();
        if (zoom < 0.01f) zoom = 0.01f;
        box_w *= zoom; box_h *= zoom;
        float pan_px = ui.getVideoPanX() * (float)ww;
        float pan_py = ui.getVideoPanY() * (float)wh;
        float cx = ww * 0.5f + pan_px;
        float cy = wh * 0.5f + pan_py;
        SDL_FRect dst;
        if (rot90) {
            dst = { cx - box_h * 0.5f, cy - box_w * 0.5f, box_h, box_w };
        } else {
            dst = { cx - box_w * 0.5f, cy - box_h * 0.5f, box_w, box_h };
        }
        if (angle == 0.0)
            SDL_RenderTexture(renderer, g_app.video_texture, nullptr, &dst);
        else
            SDL_RenderTextureRotated(renderer, g_app.video_texture, nullptr, &dst,
                                     angle, nullptr, SDL_FLIP_NONE);
    }

    ui.newFrame();
    ui.render(ww, wh);

    SDL_RenderPresent(renderer);
}

#ifdef _WIN32
// Timer id used to drive rendering during the modal move/size loop.
// Arbitrary but unique-within-window value.
constexpr UINT_PTR kModalLoopTimerId = 0x42414E41;   // 'BANA'

bool SDLCALL modalRenderMessageHook(void* userdata, MSG* msg) {
    (void)userdata;
    if (msg && msg->message == WM_ENTERSIZEMOVE) {
        // ~60 fps tick. Windows caps timer resolution at ~15 ms so this
        // effectively runs at whatever the system granularity is - fine
        // for keeping the picture live during a drag.
        SetTimer(msg->hwnd, kModalLoopTimerId, USER_TIMER_MINIMUM, nullptr);
    } else if (msg && msg->message == WM_EXITSIZEMOVE) {
        KillTimer(msg->hwnd, kModalLoopTimerId);
    } else if (msg && msg->message == WM_TIMER
               && msg->wParam == kModalLoopTimerId) {
        renderOneFrame();
    }
    return true;   // let SDL / DefWindowProc handle the message normally
}
#endif

}   // anonymous namespace

int main(int argc, char* argv[]) {
    // Smoke-test flag. Used by CI + local `install_local.ps1` follow-ups
    // to verify the built exe can actually get through SDL init, window
    // creation, ImGui bootstrap, font atlas load, UIController ctor
    // (which reads every sidecar), and a couple of render frames -
    // then exit 0. If any of that crashes, exit code is non-zero and
    // CI catches the regression before it reaches users.
    //   BananaPlayer.exe --check
    // Skips single-instance handoff (would falsely forward + exit)
    // and skips the interactive event loop.
    bool smoke_check = false;
    for (int i = 1; i < argc; ++i) {
        if (argv[i] && (std::strcmp(argv[i], "--check") == 0
                     || std::strcmp(argv[i], "--smoke-test") == 0)) {
            smoke_check = true;
            break;
        }
    }

    // Headless audio-dump mode. Writes the exact post-filter + post-swr
    // S16 stereo PCM that would be handed to SDL as a WAV file, then
    // exits. Used to self-verify audio artifacts (dynaudnorm clipping,
    // resampler zipper noise, etc.) without needing to listen.
    //   BananaPlayer.exe --audio-dump OUT.wav [--audio-dump-seconds N]
    //     [--audio-dump-start SEC] [--audio-dump-normalize]
    //     [--audio-dump-voiceboost] INPUT
    // Bypasses SDL video / window / UIController entirely; opens an
    // audio-only dummy SDL device so VideoPlayer's push path runs.
    const char* dump_path     = nullptr;
    const char* dump_input    = nullptr;
    double      dump_seconds  = 30.0;
    double      dump_start    = 0.0;
    bool        dump_loudness = false;
    bool        dump_voice    = false;
    std::vector<const char*> positional;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!a) continue;
        if (std::strcmp(a, "--audio-dump") == 0 && i + 1 < argc) {
            dump_path = argv[++i];
        } else if (std::strcmp(a, "--audio-dump-seconds") == 0 && i + 1 < argc) {
            dump_seconds = std::atof(argv[++i]);
            if (dump_seconds <= 0.0) dump_seconds = 30.0;
        } else if (std::strcmp(a, "--audio-dump-start") == 0 && i + 1 < argc) {
            dump_start = std::atof(argv[++i]);
            if (dump_start < 0.0) dump_start = 0.0;
        } else if (std::strcmp(a, "--audio-dump-normalize") == 0) {
            dump_loudness = true;
        } else if (std::strcmp(a, "--audio-dump-voiceboost") == 0) {
            dump_voice = true;
        } else if (a[0] != '-') {
            positional.push_back(a);
        }
    }
    if (dump_path) {
        if (positional.empty()) {
            std::fprintf(stderr, "--audio-dump requires an INPUT file\n");
            return 2;
        }
        dump_input = positional.front();

        // Dummy SDL audio driver: opens a device that accepts samples
        // but never drains them (we don't care - the tap captures the
        // bytes before SDL even sees them). Must be set BEFORE SDL_Init.
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
        if (!SDL_Init(SDL_INIT_AUDIO)) {
            std::fprintf(stderr, "SDL_Init(AUDIO) failed: %s\n", SDL_GetError());
            return 3;
        }

        // WAV: write a 44-byte RIFF header with placeholder sizes, then
        // append raw PCM, then rewind and patch sizes on exit. S16LE
        // stereo at the source sample rate (set after loadFile).
        FILE* fp = std::fopen(dump_path, "wb");
        if (!fp) {
            std::fprintf(stderr, "cannot open %s for writing\n", dump_path);
            SDL_Quit();
            return 4;
        }

        VideoPlayer dp_player;
        // Discard video packets at demux so the video queue never
        // back-pressures the audio decode path in headless mode.
        dp_player.setDemuxDiscardVideo(true);
        // Dummy SDL driver never drains the audio stream, so disable
        // the normal "sleep when SDL queue > 768KB" back-pressure.
        dp_player.setDemuxIgnoreAudioQueue(true);
        if (!dp_player.loadFile(dump_input)) {
            std::fprintf(stderr, "loadFile failed: %s\n", dump_input);
            std::fclose(fp);
            SDL_Quit();
            return 5;
        }
        int rate = dp_player.getAudioSampleRate();
        if (rate <= 0) {
            std::fprintf(stderr, "no audio stream in %s\n", dump_input);
            std::fclose(fp);
            SDL_Quit();
            return 6;
        }
        const int channels = 2;                 // VideoPlayer hardcodes stereo
        const int bits     = 16;
        const int byte_rate   = rate * channels * (bits / 8);
        const int block_align = channels * (bits / 8);

        auto write_u32_le = [&](uint32_t v) {
            uint8_t b[4] = { (uint8_t)(v & 0xff), (uint8_t)((v >> 8) & 0xff),
                             (uint8_t)((v >> 16) & 0xff), (uint8_t)((v >> 24) & 0xff) };
            std::fwrite(b, 1, 4, fp);
        };
        auto write_u16_le = [&](uint16_t v) {
            uint8_t b[2] = { (uint8_t)(v & 0xff), (uint8_t)((v >> 8) & 0xff) };
            std::fwrite(b, 1, 2, fp);
        };
        std::fwrite("RIFF", 1, 4, fp); write_u32_le(0);          // size patched later
        std::fwrite("WAVE", 1, 4, fp);
        std::fwrite("fmt ", 1, 4, fp); write_u32_le(16);
        write_u16_le(1);                                          // PCM
        write_u16_le((uint16_t)channels);
        write_u32_le((uint32_t)rate);
        write_u32_le((uint32_t)byte_rate);
        write_u16_le((uint16_t)block_align);
        write_u16_le((uint16_t)bits);
        std::fwrite("data", 1, 4, fp); write_u32_le(0);          // size patched later

        const uint64_t target_bytes = (uint64_t)(dump_seconds * byte_rate);
        std::atomic<uint64_t> written{0};
        std::mutex fp_mutex;
        dp_player.setAudioTap([&](const uint8_t* data, int n) {
            uint64_t already = written.load();
            if (already >= target_bytes) return;
            int to_write = n;
            if (already + (uint64_t)to_write > target_bytes) {
                to_write = (int)(target_bytes - already);
            }
            {
                std::lock_guard<std::mutex> lk(fp_mutex);
                std::fwrite(data, 1, (size_t)to_write, fp);
            }
            written.fetch_add((uint64_t)to_write);
        });

        if (dump_loudness) dp_player.setLoudnessNormalize(true);
        if (dump_voice)    dp_player.setVoiceBoost(true);
        if (dump_start > 0.0) dp_player.seek(dump_start);
        dp_player.play();

        // Spin until the tap has captured enough. Timeout at 60 s wall
        // so a stuck decoder doesn't hang the process.
        Uint64 start_ms = SDL_GetTicks();
        while (written.load() < target_bytes) {
            Uint64 elapsed = SDL_GetTicks() - start_ms;
            if (elapsed > 60000) {
                std::fprintf(stderr, "timeout after %llu ms, got %llu / %llu bytes\n",
                             (unsigned long long)elapsed,
                             (unsigned long long)written.load(),
                             (unsigned long long)target_bytes);
                break;
            }
            SDL_Delay(20);
        }

        // Clear tap before player destruction so the decode thread
        // can't fire into a dangling fp during teardown.
        dp_player.setAudioTap({});
        dp_player.unload();

        // Patch WAV header sizes in place.
        uint64_t data_bytes  = written.load();
        uint32_t riff_size   = (uint32_t)(36 + data_bytes);
        std::fflush(fp);
        std::fseek(fp, 4, SEEK_SET); write_u32_le(riff_size);
        std::fseek(fp, 40, SEEK_SET); write_u32_le((uint32_t)data_bytes);
        std::fclose(fp);
        std::fprintf(stdout,
            "wrote %s  rate=%d ch=%d bytes=%llu seconds=%.3f  normalize=%d voiceboost=%d start=%.1f\n",
            dump_path, rate, channels,
            (unsigned long long)data_bytes,
            (double)data_bytes / (double)byte_rate,
            dump_loudness ? 1 : 0, dump_voice ? 1 : 0, dump_start);
        SDL_Quit();
        return 0;
    }
#ifdef _WIN32
    // Single-instance handoff. Read the persisted setting BEFORE SDL
    // init so the "forward + exit" fast path skips window creation.
    // (Skipped in smoke-test mode - we want a real init path here.)
    bool single_instance_wanted = false;
    if (!smoke_check) {
        single_instance_wanted = SingleInstance::peekPolicyFromConfig();
    }
    if (single_instance_wanted) {
        if (!SingleInstance::activate()) {
            // Another instance already primary. Forward each arg
            // through the pipe, then exit without opening a window.
            bool any_forwarded = false;
            for (int i = 1; i < argc; ++i) {
                if (argv[i] && argv[i][0]) {
                    any_forwarded |= SingleInstance::forwardPathToPrimary(argv[i]);
                }
            }
            // If forwarding failed for any reason, fall through to a
            // fresh window so the user isn't left with nothing.
            if (argc <= 1 || any_forwarded) return 0;
        }
    }
#endif

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
#ifdef _WIN32
        SingleInstance::deactivate();
#endif
        return -1;
    }

#ifdef _WIN32
    // Register the load-path event after SDL_Init so registerSdlEvent
    // gets a valid id. Safe to call whether or not we're primary; the
    // event id is used both by our own event loop and by the pipe
    // listener SingleInstance::activate() spins up (or spins up later
    // if the user toggles the setting on mid-session).
    SingleInstance::registerSdlEvent();
#endif

    SDL_Window* window = SDL_CreateWindow("Banana Player",
                                          1280, 720,
                                          SDL_WINDOW_RESIZABLE
                                              | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return -1;
    }
    SDL_SetWindowMinimumSize(window, 640, 400);

#ifdef _WIN32
    // Pin the embedded exe icon onto this window so it appears in the title
    // bar and Alt-Tab preview. Explorer / taskbar pick it up automatically
    // from the .rc-embedded resource.
    {
        HICON hIconBig   = (HICON)LoadImageW(GetModuleHandleW(nullptr),
                                             MAKEINTRESOURCEW(IDI_APPICON),
                                             IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
        HICON hIconSmall = (HICON)LoadImageW(GetModuleHandleW(nullptr),
                                             MAKEINTRESOURCEW(IDI_APPICON),
                                             IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
        SDL_PropertiesID props = SDL_GetWindowProperties(window);
        HWND hwnd = (HWND)SDL_GetPointerProperty(props,
            SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        if (hwnd) {
            if (hIconBig)   SendMessageW(hwnd, WM_SETICON, ICON_BIG,   (LPARAM)hIconBig);
            if (hIconSmall) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);
        }
    }
#endif

    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return -1;
    }
    SDL_SetRenderVSync(renderer, 1);

    // ImGui setup
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    // Do NOT enable NavEnableKeyboard: with it on, clicked overlay buttons
    // retain focus and steal Space/Enter, breaking Space=play/pause and
    // making shortcuts re-trigger whichever button was last clicked.

    Style::LoadFonts(1.0f);
    Style::SetupDarkTheme();

    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    VideoPlayer player;
    UIController ui(&player, window, renderer);

    bool quit = false;
    ui.onExitRequested = [&]() { quit = true; };
    // Smoke test: render a few frames then exit. Verifies the whole
    // init chain (SDL video/audio/window/renderer + ImGui + font atlas
    // + UIController ctor + all sidecar loads + Promo start + splash
    // draw) completes without crashing. Any exception / assertion
    // failure surfaces as non-zero exit code.
    int smoke_frames_remaining = smoke_check ? 3 : 0;

    // Publish the render state to the file-scope AppRenderCtx so
    // renderOneFrame() (called both from the main loop and from the
    // modal-loop timer hook) can drive rendering off the same
    // instance. All ownership stays on the stack here; g_app just
    // borrows the pointers.
    g_app.window   = window;
    g_app.renderer = renderer;
    g_app.player   = &player;
    g_app.ui       = &ui;
    g_app.video_texture   = nullptr;
    g_app.tex_w = 0; g_app.tex_h = 0;
    g_app.has_first_frame = false;

#ifdef _WIN32
    // Route WM_ENTERSIZEMOVE / WM_TIMER / WM_EXITSIZEMOVE through our
    // hook so the picture keeps updating while the user drags the
    // title bar or resizes the edge (Windows normally freezes the
    // app's message loop for the duration of that modal loop).
    SDL_SetWindowsMessageHook(modalRenderMessageHook, nullptr);
#endif

    // Screensaver / monitor-sleep suppression while a video is actually
    // playing. Anything else (paused, no file, minimized) lets Windows
    // do its thing so the user's normal idle timers still work.
    SDL_EnableScreenSaver();
    bool screensaver_disabled = false;

    if (argc > 1) {
        // A single arg that looks like a YouTube page URL takes the
        // yt-dlp resolve path (openAny) - not enqueuePaths, whose
        // GetFileAttributesA / directory-expansion has nothing useful
        // to do with a URL and would fall through to loadFile which
        // FFmpeg can't open for youtube.com HTML.
        if (argc == 2 && UIController::isYoutubeUrl(argv[1])) {
            ui.openAny(argv[1]);
        } else {
            // Any extra args become a playlist; the first one starts playing.
            std::vector<std::string> paths;
            paths.reserve(argc - 1);
            for (int i = 1; i < argc; ++i) paths.emplace_back(argv[i]);
            ui.enqueuePaths(paths, /*play_first=*/true);
        }
    }

    while (!quit) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                quit = true;
            }
#ifdef _WIN32
            else if (SingleInstance::sdlEventType() != (Uint32)-1
                     && event.type == SingleInstance::sdlEventType()) {
                // A forwarded path arrived from a secondary launch via
                // the named pipe. Bring our window to the front so the
                // user sees the file open instead of wondering where
                // Explorer's double-click went.
                //
                // The secondary process called AllowSetForegroundWindow(ASFW_ANY)
                // right before the pipe write so SetForegroundWindow below
                // actually promotes us to foreground instead of just
                // flashing the taskbar entry.
                auto* p = static_cast<std::string*>(event.user.data1);
                if (p) {
                    // Restore first if minimized - otherwise Raise/SetForeground
                    // just brings the taskbar entry forward, not the actual
                    // window pixels.
                    SDL_WindowFlags flags = SDL_GetWindowFlags(window);
                    if (flags & SDL_WINDOW_MINIMIZED) SDL_RestoreWindow(window);
                    SDL_RaiseWindow(window);
                    SDL_PropertiesID props = SDL_GetWindowProperties(window);
                    HWND hwnd = (HWND)SDL_GetPointerProperty(props,
                        SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
                    if (hwnd) {
                        // Belt + suspenders. SDL_RaiseWindow already calls
                        // SetForegroundWindow but the sequence + BringWindowToTop
                        // is what actually pulls the window pixels over the
                        // previous foreground app on Windows 10/11.
                        BringWindowToTop(hwnd);
                        SetForegroundWindow(hwnd);
                    }
                    ui.openAny(*p);
                    delete p;
                }
            }
#endif
            else {
                ui.handleEvent(event);
            }
        }

        // Screensaver toggle - track edges so we only call SDL when the
        // playing state actually changes (SDL calls are cheap but this
        // stays obvious in a profile if we ever wonder).
        bool want_ss_off = player.isPlaying();
        if (want_ss_off && !screensaver_disabled) {
            SDL_DisableScreenSaver();
            screensaver_disabled = true;
        } else if (!want_ss_off && screensaver_disabled) {
            SDL_EnableScreenSaver();
            screensaver_disabled = false;
        }

        // Draw one frame. Extracted into a function so the modal-loop
        // Windows message hook can call the same code path while the
        // user is dragging the title bar or resizing the window edge.
        renderOneFrame();

        // Smoke-test countdown. Every rendered frame we get past has
        // exercised the same code paths a normal user would; three is
        // enough to catch init-time crashes without hanging CI.
        if (smoke_frames_remaining > 0) {
            if (--smoke_frames_remaining == 0) {
                std::fprintf(stderr, "smoke-test: OK (3 frames rendered)\n");
                quit = true;
            }
        }
    }

    if (g_app.video_texture) SDL_DestroyTexture(g_app.video_texture);
    g_app.video_texture = nullptr;

    // Free UI-owned SDL textures (thumbnail cache) before the renderer is
    // destroyed. UIController itself is stack-scoped to end-of-main, so
    // its destructor runs AFTER SDL_Quit - too late for texture cleanup.
    ui.shutdownBeforeSdl();

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

#ifdef _WIN32
    SingleInstance::deactivate();
#endif
    return 0;
}
