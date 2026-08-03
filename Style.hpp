#pragma once

#include "imgui.h"

namespace Style {

enum class Icon {
    Play,
    Pause,
    Stop,
    Prev,
    Next,
    SeekBack,
    SeekForward,
    Fullscreen,
    FullscreenExit,
    Volume,
    VolumeLow,
    Mute,
    Settings,
    Snapshot,
    Loop,
    Open,
    Playlist,
    Menu,
    Close,
    StepBack,
    StepForward,
    Info,
    Subtitle,   // CC-style rectangle with two subtitle lines
};

void SetupDarkTheme();
void LoadFonts(float ui_scale = 1.0f);

// Subtitle font presets. LoadFonts populates each of these; a nullptr
// return means the font wasn't available on this system - callers
// should fall back to the default. Kept small: adding more presets
// each requires an extra atlas slot and startup work.
enum class SubtitleFont { Default, MalgunBold, Gulim, Batang };
ImFont* GetSubtitleFont(SubtitleFont f);

// Draw a stateless icon at `center` within the current window's foreground draw list.
void DrawIcon(ImDrawList* dl, ImVec2 center, float size, Icon icon, ImU32 color);

// Full icon button (rounded background + hover + optional badge).
bool IconButton(const char* id, Icon icon, const ImVec2& size,
                const char* tooltip = nullptr, bool active = false,
                float scale = 1.0f, float alpha = 1.0f);

// Multiplies the alpha byte of `col` by `alpha`.
inline ImU32 Fade(ImU32 col, float alpha) {
    int a = (col >> IM_COL32_A_SHIFT) & 0xFF;
    a = (int)(a * alpha);
    if (a < 0) a = 0; else if (a > 255) a = 255;
    return (col & ~IM_COL32_A_MASK) | ((ImU32)a << IM_COL32_A_SHIFT);
}

// Text button with rounded background matching the theme.
bool TextButton(const char* label, const ImVec2& size, bool active = false);

// Draws a smooth translucent gradient rectangle (top->bottom).
void GradientRect(ImDrawList* dl, ImVec2 tl, ImVec2 br, ImU32 top_col, ImU32 bot_col);

// Draw a modern-looking horizontal progress/seek bar. Returns true if the user
// changed the value while dragging. `time`/`duration` in seconds.
bool SeekBar(const char* id, float& value, float min, float max,
             float width, float height, bool* out_held = nullptr);

// Simple pill/badge with text.
void Badge(const char* text, ImU32 bg, ImU32 fg);

} // namespace Style
