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
        if (vw > 0 && vh > 0
            && (vw != g_app.tex_w || vh != g_app.tex_h
                || !g_app.video_texture
                || ui_gen != g_app.tex_generation)) {
            if (g_app.video_texture) SDL_DestroyTexture(g_app.video_texture);
            // Property-based creation so we can pin the input
            // colorspace / range. Default full-range BT.709; the
            // Studio-range toggle in Settings -> Video switches to
            // BT.709 LIMITED (16-235). SDL's own default for YUV
            // textures is BT.601 LIMITED, which is neither what we
            // want as the default nor what most modern content is.
            SDL_PropertiesID props = SDL_CreateProperties();
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER,
                                  SDL_PIXELFORMAT_IYUV);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER,
                                  SDL_TEXTUREACCESS_STREAMING);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, vw);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, vh);
            SDL_Colorspace cs = ui.getYuvStudioRange()
                              ? SDL_COLORSPACE_BT709_LIMITED
                              : SDL_COLORSPACE_BT709_FULL;
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER,
                                  (Sint64)cs);
            g_app.video_texture = SDL_CreateTextureWithProperties(renderer, props);
            SDL_DestroyProperties(props);
            if (g_app.video_texture)
                SDL_SetTextureScaleMode(g_app.video_texture, SDL_SCALEMODE_LINEAR);
            g_app.tex_w = vw; g_app.tex_h = vh;
            g_app.tex_generation = ui_gen;
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
        float src_aspect = (float)g_app.tex_w / (float)g_app.tex_h;
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
    // License key generation mode. Server-side use: takes an email
    // (after payment) and prints the matching Pro key to stdout so
    // the delivery pipeline can email it to the buyer. Never runs
    // any SDL / UI code - pure crypto + print.
    //   BananaPlayer.exe --gen-license user@example.com
    for (int i = 1; i < argc - 1; ++i) {
        if (argv[i] && std::strcmp(argv[i], "--gen-license") == 0) {
            // Release builds link as WINDOWS subsystem; stdout redirection
            // from cmd/pwsh is unreliable. Write to a file arg instead:
            //   BananaPlayer.exe --gen-license user@example.com key.txt
            // The file receives one line with the key (no trailing LF).
            const char* email = argv[i + 1];
            std::string k = License::keyForEmail(email);
            if (k.empty()) return 2;
            const char* out_path = (i + 2 < argc) ? argv[i + 2] : nullptr;
            if (!out_path) return 3;
            std::FILE* f = std::fopen(out_path, "wb");
            if (!f) return 4;
            std::fwrite(k.data(), 1, k.size(), f);
            std::fclose(f);
            return 0;
        }
    }
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
