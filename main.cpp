#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "VideoPlayer.hpp"
#include "UIController.hpp"
#include "Style.hpp"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    // Must match IDI_APPICON in resources/app.rc.
    #define IDI_APPICON 101
#endif

int main(int argc, char* argv[]) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    SDL_Window* window = SDL_CreateWindow("Vela",
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

    SDL_Texture* video_texture = nullptr;
    int tex_w = 0, tex_h = 0;
    // Streaming YUV textures are created uninitialized; on many GPUs the
    // memory shows up as neon green until the first frame is uploaded.
    // Gate the draw on "we have received at least one frame" so we show
    // the app's clear color during the tiny window between file-load and
    // first-decoded-frame instead of that green flash.
    bool has_first_frame = false;

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
            } else {
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

        // Ensure texture matches source dims
        if (player.isLoaded()) {
            int vw = player.getVideoWidth();
            int vh = player.getVideoHeight();
            if (vw > 0 && vh > 0 && (vw != tex_w || vh != tex_h || !video_texture)) {
                if (video_texture) SDL_DestroyTexture(video_texture);
                video_texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV,
                                                  SDL_TEXTUREACCESS_STREAMING, vw, vh);
                if (video_texture) SDL_SetTextureScaleMode(video_texture, SDL_SCALEMODE_LINEAR);
                tex_w = vw; tex_h = vh;
                has_first_frame = false;   // wait for the first real upload
            }
        }

        if (video_texture) {
            if (player.updateTexture(video_texture)) has_first_frame = true;
        }

        // Render
        SDL_SetRenderDrawColor(renderer, 6, 8, 14, 255);
        SDL_RenderClear(renderer);

        int ww = 0, wh = 0;
        SDL_GetWindowSize(window, &ww, &wh);

        if (video_texture && tex_w > 0 && tex_h > 0 && has_first_frame) {
            // Fit to window preserving the source aspect. No forced-aspect
            // modes - forcing 16:9/4:3/21:9 on a source that isn't that shape
            // just distorts the picture, and Stretch adds nothing over Fit
            // that isn't the same distortion in a different flavor.
            float src_aspect = (float)tex_w / (float)tex_h;

            // Intrinsic rotation from the container's display matrix
            // (portrait phone videos are landscape pixels tagged 90 deg).
            // Stacks with the user's Ctrl+R rotation - intrinsic first,
            // then user - so the base orientation is always correct and
            // the user can nudge further from there.
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

            // Effective aspect after rotation: swapped for 90/270.
            float eff_aspect = rot90 ? (1.0f / src_aspect) : src_aspect;
            float dst_aspect = (float)ww / (float)wh;
            float box_w, box_h;
            if (dst_aspect > eff_aspect) {
                box_h = (float)wh;
                box_w = wh * eff_aspect;
            } else {
                box_w = (float)ww;
                box_h = ww / eff_aspect;
            }
            float cx = ww * 0.5f;
            float cy = wh * 0.5f;
            SDL_FRect dst;
            if (rot90) {
                // Pre-rotation shape has width/height swapped so that
                // its 90 deg-rotated bounding box equals (box_w, box_h).
                dst = { cx - box_h * 0.5f, cy - box_w * 0.5f, box_h, box_w };
            } else {
                dst = { cx - box_w * 0.5f, cy - box_h * 0.5f, box_w, box_h };
            }
            if (angle == 0.0) {
                SDL_RenderTexture(renderer, video_texture, nullptr, &dst);
            } else {
                SDL_RenderTextureRotated(renderer, video_texture, nullptr, &dst,
                                         angle, nullptr, SDL_FLIP_NONE);
            }
        }

        ui.newFrame();
        ui.render(ww, wh);

        SDL_RenderPresent(renderer);
    }

    if (video_texture) SDL_DestroyTexture(video_texture);

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
    return 0;
}
