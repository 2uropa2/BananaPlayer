#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "VideoPlayer.hpp"
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
    if (player.isLoaded()) {
        int vw = player.getOutputWidth();
        int vh = player.getOutputHeight();
        if (vw > 0 && vh > 0
            && (vw != g_app.tex_w || vh != g_app.tex_h || !g_app.video_texture)) {
            if (g_app.video_texture) SDL_DestroyTexture(g_app.video_texture);
            g_app.video_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV,
                                                    SDL_TEXTUREACCESS_STREAMING, vw, vh);
            if (g_app.video_texture)
                SDL_SetTextureScaleMode(g_app.video_texture, SDL_SCALEMODE_LINEAR);
            g_app.tex_w = vw; g_app.tex_h = vh;
            g_app.has_first_frame = false;
        }
    }
    if (g_app.video_texture) {
        if (player.updateTexture(g_app.video_texture)) g_app.has_first_frame = true;
    }

    SDL_SetRenderDrawColor(renderer, 6, 8, 14, 255);
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
constexpr UINT_PTR kModalLoopTimerId = 0x56454C41;   // 'VELA'

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
#ifdef _WIN32
    // Single-instance handoff. Read the persisted setting BEFORE SDL
    // init so the "forward + exit" fast path skips window creation.
    bool single_instance_wanted = SingleInstance::peekPolicyFromConfig();
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
                auto* p = static_cast<std::string*>(event.user.data1);
                if (p) {
                    SDL_RaiseWindow(window);
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
