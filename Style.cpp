#include "Style.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Style {

static constexpr float kPI = 3.14159265358979323846f;

void SetupDarkTheme() {
    ImGuiStyle& s = ImGui::GetStyle();

    s.WindowRounding    = 10.0f;
    s.ChildRounding     = 8.0f;
    s.FrameRounding     = 6.0f;
    s.PopupRounding     = 8.0f;
    s.ScrollbarRounding = 6.0f;
    s.GrabRounding      = 6.0f;
    s.TabRounding       = 6.0f;
    s.WindowBorderSize  = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.PopupBorderSize   = 1.0f;

    s.WindowPadding     = ImVec2(14, 12);
    s.FramePadding      = ImVec2(10, 6);
    s.ItemSpacing       = ImVec2(10, 8);
    s.ItemInnerSpacing  = ImVec2(6, 6);
    s.CellPadding       = ImVec2(6, 4);
    s.IndentSpacing     = 20.0f;
    s.ScrollbarSize     = 14.0f;
    s.GrabMinSize       = 10.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                  = ImVec4(0.94f, 0.94f, 0.96f, 1.00f);
    c[ImGuiCol_TextDisabled]          = ImVec4(0.48f, 0.48f, 0.55f, 1.00f);
    c[ImGuiCol_WindowBg]              = ImVec4(0.06f, 0.06f, 0.09f, 0.96f);
    c[ImGuiCol_ChildBg]               = ImVec4(0.05f, 0.05f, 0.07f, 0.90f);
    c[ImGuiCol_PopupBg]               = ImVec4(0.08f, 0.08f, 0.11f, 0.98f);
    c[ImGuiCol_Border]                = ImVec4(0.20f, 0.22f, 0.30f, 0.55f);
    c[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_FrameBg]               = ImVec4(0.12f, 0.13f, 0.18f, 0.85f);
    c[ImGuiCol_FrameBgHovered]        = ImVec4(0.18f, 0.20f, 0.28f, 0.90f);
    c[ImGuiCol_FrameBgActive]         = ImVec4(0.24f, 0.28f, 0.42f, 0.95f);
    c[ImGuiCol_TitleBg]               = ImVec4(0.05f, 0.05f, 0.07f, 1.00f);
    c[ImGuiCol_TitleBgActive]         = ImVec4(0.10f, 0.11f, 0.16f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.05f, 0.05f, 0.07f, 0.75f);
    c[ImGuiCol_MenuBarBg]             = ImVec4(0.07f, 0.07f, 0.10f, 0.98f);
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0.02f, 0.02f, 0.04f, 0.50f);
    c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.28f, 0.30f, 0.42f, 0.80f);
    c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.38f, 0.42f, 0.60f, 0.90f);
    c[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.48f, 0.55f, 0.85f, 1.00f);
    c[ImGuiCol_CheckMark]             = ImVec4(0.55f, 0.75f, 1.00f, 1.00f);
    c[ImGuiCol_SliderGrab]            = ImVec4(0.55f, 0.75f, 1.00f, 1.00f);
    c[ImGuiCol_SliderGrabActive]      = ImVec4(0.75f, 0.90f, 1.00f, 1.00f);
    c[ImGuiCol_Button]                = ImVec4(0.14f, 0.15f, 0.22f, 0.90f);
    c[ImGuiCol_ButtonHovered]         = ImVec4(0.24f, 0.30f, 0.50f, 0.95f);
    c[ImGuiCol_ButtonActive]          = ImVec4(0.34f, 0.44f, 0.72f, 1.00f);
    c[ImGuiCol_Header]                = ImVec4(0.18f, 0.22f, 0.34f, 0.85f);
    c[ImGuiCol_HeaderHovered]         = ImVec4(0.26f, 0.32f, 0.50f, 0.90f);
    c[ImGuiCol_HeaderActive]          = ImVec4(0.34f, 0.44f, 0.72f, 1.00f);
    c[ImGuiCol_Separator]             = ImVec4(0.20f, 0.22f, 0.30f, 0.50f);
    c[ImGuiCol_SeparatorHovered]      = ImVec4(0.34f, 0.44f, 0.72f, 0.78f);
    c[ImGuiCol_SeparatorActive]       = ImVec4(0.50f, 0.65f, 1.00f, 1.00f);
    c[ImGuiCol_ResizeGrip]            = ImVec4(0.20f, 0.24f, 0.36f, 0.30f);
    c[ImGuiCol_ResizeGripHovered]     = ImVec4(0.34f, 0.44f, 0.72f, 0.78f);
    c[ImGuiCol_ResizeGripActive]      = ImVec4(0.50f, 0.65f, 1.00f, 1.00f);
    c[ImGuiCol_Tab]                   = ImVec4(0.12f, 0.14f, 0.20f, 0.86f);
    c[ImGuiCol_TabHovered]            = ImVec4(0.28f, 0.36f, 0.56f, 0.86f);
    c[ImGuiCol_PlotLines]             = ImVec4(0.65f, 0.72f, 0.98f, 1.00f);
    c[ImGuiCol_PlotLinesHovered]      = ImVec4(0.86f, 0.90f, 1.00f, 1.00f);
    c[ImGuiCol_PlotHistogram]         = ImVec4(0.55f, 0.75f, 1.00f, 1.00f);
    c[ImGuiCol_PlotHistogramHovered]  = ImVec4(0.75f, 0.90f, 1.00f, 1.00f);
    c[ImGuiCol_TextSelectedBg]        = ImVec4(0.30f, 0.42f, 0.72f, 0.55f);
    c[ImGuiCol_DragDropTarget]        = ImVec4(0.75f, 0.90f, 1.00f, 0.90f);
    c[ImGuiCol_NavCursor]             = ImVec4(0.55f, 0.75f, 1.00f, 1.00f);
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.75f);
    c[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.80f, 0.80f, 0.80f, 0.24f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.03f, 0.03f, 0.06f, 0.55f);
}

void LoadFonts(float ui_scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    ImFontConfig cfg;
    cfg.OversampleH = 3;
    cfg.OversampleV = 2;
    cfg.PixelSnapH  = false;

    const float base = 16.0f * ui_scale;
    const char* segoe = "C:\\Windows\\Fonts\\segoeui.ttf";
    // Malgun Gothic ships with every modern Windows install and covers
    // Korean glyphs used in .srt / .smi subtitles.
    const char* malgun = "C:\\Windows\\Fonts\\malgun.ttf";

    ImFont* main_font = io.Fonts->AddFontFromFileTTF(segoe, base, &cfg,
                                                     io.Fonts->GetGlyphRangesDefault());
    if (!main_font) {
        io.Fonts->AddFontDefault();
    }

    // Merge Korean glyphs on top of the main font so any text renderer picks
    // up hangul without swapping fonts explicitly.
    ImFontConfig merge;
    merge.MergeMode = true;
    merge.OversampleH = 2;
    merge.OversampleV = 2;
    merge.PixelSnapH  = false;
    io.Fonts->AddFontFromFileTTF(malgun, base, &merge,
                                 io.Fonts->GetGlyphRangesKorean());
}

void GradientRect(ImDrawList* dl, ImVec2 tl, ImVec2 br, ImU32 top_col, ImU32 bot_col) {
    dl->AddRectFilledMultiColor(tl, br, top_col, top_col, bot_col, bot_col);
}

static void drawGear(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    const int teeth = 8;
    const float outer = s * 0.95f;
    const float inner = s * 0.62f;
    const float hub   = s * 0.30f;
    for (int i = 0; i < teeth * 2; ++i) {
        float a0 = (float)i * kPI / teeth;
        float a1 = (float)(i + 1) * kPI / teeth;
        float r0 = (i % 2 == 0) ? outer : inner;
        float r1 = (i % 2 == 0) ? outer : inner;
        ImVec2 p0(c.x + std::cos(a0) * r0, c.y + std::sin(a0) * r0);
        ImVec2 p1(c.x + std::cos(a1) * r1, c.y + std::sin(a1) * r1);
        dl->AddTriangleFilled(c, p0, p1, col);
    }
    dl->AddCircleFilled(c, hub, IM_COL32(30, 32, 42, 255), 24);
    dl->AddCircle(c, hub, col, 24, std::max(1.5f, s * 0.14f));
}

void DrawIcon(ImDrawList* dl, ImVec2 c, float s, Icon icon, ImU32 col) {
    const float t = std::max(1.6f, s * 0.16f);

    switch (icon) {
    case Icon::Play: {
        ImVec2 p1(c.x - s * 0.38f, c.y - s * 0.62f);
        ImVec2 p2(c.x - s * 0.38f, c.y + s * 0.62f);
        ImVec2 p3(c.x + s * 0.62f, c.y);
        dl->AddTriangleFilled(p1, p2, p3, col);
        break;
    }
    case Icon::Pause: {
        float bw  = s * 0.22f;
        float bh  = s * 1.1f;
        float gap = s * 0.18f;
        dl->AddRectFilled(ImVec2(c.x - gap - bw, c.y - bh * 0.5f),
                          ImVec2(c.x - gap,      c.y + bh * 0.5f), col, bw * 0.35f);
        dl->AddRectFilled(ImVec2(c.x + gap,      c.y - bh * 0.5f),
                          ImVec2(c.x + gap + bw, c.y + bh * 0.5f), col, bw * 0.35f);
        break;
    }
    case Icon::Stop: {
        float ss = s * 0.95f;
        dl->AddRectFilled(ImVec2(c.x - ss * 0.5f, c.y - ss * 0.5f),
                          ImVec2(c.x + ss * 0.5f, c.y + ss * 0.5f), col, 3.0f);
        break;
    }
    case Icon::Prev: {
        dl->AddRectFilled(ImVec2(c.x - s * 0.75f, c.y - s * 0.62f),
                          ImVec2(c.x - s * 0.55f, c.y + s * 0.62f), col, 1.5f);
        ImVec2 p1(c.x + s * 0.55f, c.y - s * 0.62f);
        ImVec2 p2(c.x + s * 0.55f, c.y + s * 0.62f);
        ImVec2 p3(c.x - s * 0.35f, c.y);
        dl->AddTriangleFilled(p1, p2, p3, col);
        break;
    }
    case Icon::Next: {
        ImVec2 p1(c.x - s * 0.55f, c.y - s * 0.62f);
        ImVec2 p2(c.x - s * 0.55f, c.y + s * 0.62f);
        ImVec2 p3(c.x + s * 0.35f, c.y);
        dl->AddTriangleFilled(p1, p2, p3, col);
        dl->AddRectFilled(ImVec2(c.x + s * 0.55f, c.y - s * 0.62f),
                          ImVec2(c.x + s * 0.75f, c.y + s * 0.62f), col, 1.5f);
        break;
    }
    case Icon::SeekBack: {
        // << two triangles pointing left
        for (int i = 0; i < 2; ++i) {
            float ox = (i - 0.5f) * s * 0.7f;
            ImVec2 p1(c.x + ox + s * 0.35f, c.y - s * 0.55f);
            ImVec2 p2(c.x + ox + s * 0.35f, c.y + s * 0.55f);
            ImVec2 p3(c.x + ox - s * 0.30f, c.y);
            dl->AddTriangleFilled(p1, p2, p3, col);
        }
        break;
    }
    case Icon::SeekForward: {
        // >> two triangles pointing right
        for (int i = 0; i < 2; ++i) {
            float ox = (i - 0.5f) * s * 0.7f;
            ImVec2 p1(c.x + ox - s * 0.35f, c.y - s * 0.55f);
            ImVec2 p2(c.x + ox - s * 0.35f, c.y + s * 0.55f);
            ImVec2 p3(c.x + ox + s * 0.30f, c.y);
            dl->AddTriangleFilled(p1, p2, p3, col);
        }
        break;
    }
    case Icon::StepBack: {
        // Bar + triangle left (like Prev)
        dl->AddRectFilled(ImVec2(c.x - s * 0.75f, c.y - s * 0.55f),
                          ImVec2(c.x - s * 0.55f, c.y + s * 0.55f), col, 1.5f);
        ImVec2 p1(c.x + s * 0.45f, c.y - s * 0.55f);
        ImVec2 p2(c.x + s * 0.45f, c.y + s * 0.55f);
        ImVec2 p3(c.x - s * 0.40f, c.y);
        dl->AddTriangleFilled(p1, p2, p3, col);
        break;
    }
    case Icon::StepForward: {
        // Triangle right + bar
        ImVec2 p1(c.x - s * 0.45f, c.y - s * 0.55f);
        ImVec2 p2(c.x - s * 0.45f, c.y + s * 0.55f);
        ImVec2 p3(c.x + s * 0.40f, c.y);
        dl->AddTriangleFilled(p1, p2, p3, col);
        dl->AddRectFilled(ImVec2(c.x + s * 0.55f, c.y - s * 0.55f),
                          ImVec2(c.x + s * 0.75f, c.y + s * 0.55f), col, 1.5f);
        break;
    }
    case Icon::Fullscreen: {
        float len = s * 0.55f;
        // Four corner brackets pointing outward
        dl->AddLine(ImVec2(c.x - s, c.y - s + len), ImVec2(c.x - s, c.y - s), col, t);
        dl->AddLine(ImVec2(c.x - s, c.y - s), ImVec2(c.x - s + len, c.y - s), col, t);
        dl->AddLine(ImVec2(c.x + s - len, c.y - s), ImVec2(c.x + s, c.y - s), col, t);
        dl->AddLine(ImVec2(c.x + s, c.y - s), ImVec2(c.x + s, c.y - s + len), col, t);
        dl->AddLine(ImVec2(c.x - s, c.y + s - len), ImVec2(c.x - s, c.y + s), col, t);
        dl->AddLine(ImVec2(c.x - s, c.y + s), ImVec2(c.x - s + len, c.y + s), col, t);
        dl->AddLine(ImVec2(c.x + s - len, c.y + s), ImVec2(c.x + s, c.y + s), col, t);
        dl->AddLine(ImVec2(c.x + s, c.y + s), ImVec2(c.x + s, c.y + s - len), col, t);
        break;
    }
    case Icon::FullscreenExit: {
        float len = s * 0.55f;
        float in = s * 0.35f;
        // Four corner brackets pointing inward (shrink)
        dl->AddLine(ImVec2(c.x - in, c.y - in + len), ImVec2(c.x - in, c.y - in), col, t);
        dl->AddLine(ImVec2(c.x - in, c.y - in), ImVec2(c.x - in + len, c.y - in), col, t);
        dl->AddLine(ImVec2(c.x + in - len, c.y - in), ImVec2(c.x + in, c.y - in), col, t);
        dl->AddLine(ImVec2(c.x + in, c.y - in), ImVec2(c.x + in, c.y - in + len), col, t);
        dl->AddLine(ImVec2(c.x - in, c.y + in - len), ImVec2(c.x - in, c.y + in), col, t);
        dl->AddLine(ImVec2(c.x - in, c.y + in), ImVec2(c.x - in + len, c.y + in), col, t);
        dl->AddLine(ImVec2(c.x + in - len, c.y + in), ImVec2(c.x + in, c.y + in), col, t);
        dl->AddLine(ImVec2(c.x + in, c.y + in), ImVec2(c.x + in, c.y + in - len), col, t);
        break;
    }
    case Icon::Volume:
    case Icon::VolumeLow:
    case Icon::Mute: {
        // Speaker body: small rectangle + trapezoid
        ImVec2 body[4] = {
            ImVec2(c.x - s * 0.9f,  c.y - s * 0.28f),
            ImVec2(c.x - s * 0.55f, c.y - s * 0.28f),
            ImVec2(c.x - s * 0.55f, c.y + s * 0.28f),
            ImVec2(c.x - s * 0.9f,  c.y + s * 0.28f),
        };
        dl->AddConvexPolyFilled(body, 4, col);
        ImVec2 cone[4] = {
            ImVec2(c.x - s * 0.55f, c.y - s * 0.28f),
            ImVec2(c.x - s * 0.10f, c.y - s * 0.75f),
            ImVec2(c.x - s * 0.10f, c.y + s * 0.75f),
            ImVec2(c.x - s * 0.55f, c.y + s * 0.28f),
        };
        dl->AddConvexPolyFilled(cone, 4, col);
        if (icon == Icon::Volume) {
            // Two sound arcs
            for (int i = 0; i < 2; ++i) {
                float r = s * (0.28f + 0.20f * (i + 1));
                dl->PathArcTo(ImVec2(c.x - s * 0.10f, c.y), r, -0.6f, 0.6f, 16);
                dl->PathStroke(col, 0, t * 0.7f);
            }
        } else if (icon == Icon::VolumeLow) {
            float r = s * 0.38f;
            dl->PathArcTo(ImVec2(c.x - s * 0.10f, c.y), r, -0.55f, 0.55f, 16);
            dl->PathStroke(col, 0, t * 0.7f);
        } else {
            // X for mute
            ImU32 xcol = IM_COL32(240, 80, 80, 255);
            dl->AddLine(ImVec2(c.x + s * 0.20f, c.y - s * 0.55f),
                        ImVec2(c.x + s * 0.90f, c.y + s * 0.15f), xcol, t);
            dl->AddLine(ImVec2(c.x + s * 0.90f, c.y - s * 0.55f),
                        ImVec2(c.x + s * 0.20f, c.y + s * 0.15f), xcol, t);
        }
        break;
    }
    case Icon::Settings: {
        drawGear(dl, c, s, col);
        break;
    }
    case Icon::Snapshot: {
        // Camera silhouette
        dl->AddRectFilled(ImVec2(c.x - s * 0.9f, c.y - s * 0.55f),
                          ImVec2(c.x + s * 0.9f, c.y + s * 0.65f), col, s * 0.15f);
        // Viewfinder bump
        dl->AddRectFilled(ImVec2(c.x - s * 0.35f, c.y - s * 0.80f),
                          ImVec2(c.x + s * 0.20f, c.y - s * 0.55f), col, s * 0.10f);
        // Inner lens (cutout)
        dl->AddCircleFilled(c, s * 0.35f, IM_COL32(20, 22, 30, 255), 28);
        dl->AddCircle(c, s * 0.35f, col, 28, t * 0.6f);
        dl->AddCircleFilled(c, s * 0.20f, col, 28);
        break;
    }
    case Icon::Loop: {
        // Circular arrow
        dl->PathArcTo(c, s * 0.65f, -kPI * 0.15f, kPI * 1.15f, 32);
        dl->PathStroke(col, 0, t);
        // Arrow head
        float a = -kPI * 0.15f;
        ImVec2 tip(c.x + std::cos(a) * s * 0.65f, c.y + std::sin(a) * s * 0.65f);
        ImVec2 p2(tip.x - s * 0.30f, tip.y - s * 0.10f);
        ImVec2 p3(tip.x - s * 0.05f, tip.y + s * 0.30f);
        dl->AddTriangleFilled(tip, p2, p3, col);
        break;
    }
    case Icon::Open: {
        // Folder icon
        ImVec2 pts[6] = {
            ImVec2(c.x - s * 0.90f, c.y - s * 0.60f),
            ImVec2(c.x - s * 0.20f, c.y - s * 0.60f),
            ImVec2(c.x + s * 0.05f, c.y - s * 0.35f),
            ImVec2(c.x + s * 0.90f, c.y - s * 0.35f),
            ImVec2(c.x + s * 0.90f, c.y + s * 0.60f),
            ImVec2(c.x - s * 0.90f, c.y + s * 0.60f),
        };
        dl->AddConvexPolyFilled(pts, 6, col);
        break;
    }
    case Icon::Playlist: {
        for (int i = 0; i < 3; ++i) {
            float y = c.y + (i - 1) * s * 0.35f;
            dl->AddCircleFilled(ImVec2(c.x - s * 0.7f, y), s * 0.10f, col, 12);
            dl->AddRectFilled(ImVec2(c.x - s * 0.45f, y - s * 0.06f),
                              ImVec2(c.x + s * 0.85f, y + s * 0.06f), col, s * 0.06f);
        }
        break;
    }
    case Icon::Menu: {
        for (int i = 0; i < 3; ++i) {
            float y = c.y + (i - 1) * s * 0.42f;
            dl->AddRectFilled(ImVec2(c.x - s * 0.85f, y - s * 0.10f),
                              ImVec2(c.x + s * 0.85f, y + s * 0.10f), col, s * 0.10f);
        }
        break;
    }
    case Icon::Close: {
        dl->AddLine(ImVec2(c.x - s * 0.65f, c.y - s * 0.65f),
                    ImVec2(c.x + s * 0.65f, c.y + s * 0.65f), col, t);
        dl->AddLine(ImVec2(c.x + s * 0.65f, c.y - s * 0.65f),
                    ImVec2(c.x - s * 0.65f, c.y + s * 0.65f), col, t);
        break;
    }
    case Icon::Info: {
        dl->AddCircle(c, s * 0.85f, col, 32, t);
        dl->AddCircleFilled(ImVec2(c.x, c.y - s * 0.45f), s * 0.12f, col, 12);
        dl->AddRectFilled(ImVec2(c.x - s * 0.10f, c.y - s * 0.15f),
                          ImVec2(c.x + s * 0.10f, c.y + s * 0.55f), col, s * 0.06f);
        break;
    }
    }
}

bool IconButton(const char* id, Icon icon, const ImVec2& size,
                const char* tooltip, bool active, float scale, float alpha) {
    ImGui::PushID(id);
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton("btn", size);
    bool hovered = ImGui::IsItemHovered();
    bool held    = ImGui::IsItemActive();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 br(cursor.x + size.x, cursor.y + size.y);

    ImU32 bg = IM_COL32(0, 0, 0, 0);
    if (active)       bg = IM_COL32(80, 108, 220, 220);
    else if (held)    bg = IM_COL32(58, 78, 160, 220);
    else if (hovered) bg = IM_COL32(60, 72, 110, 190);
    bg = Fade(bg, alpha);
    if ((bg >> IM_COL32_A_SHIFT) != 0) {
        float rounding = std::min(size.x, size.y) * 0.28f;
        dl->AddRectFilled(cursor, br, bg, rounding);
    }

    ImVec2 center(cursor.x + size.x * 0.5f, cursor.y + size.y * 0.5f);
    float icon_size = std::min(size.x, size.y) * 0.38f * scale;
    ImU32 fg = active ? IM_COL32(255, 255, 255, 255) : IM_COL32(232, 232, 244, 255);
    DrawIcon(dl, center, icon_size, icon, Fade(fg, alpha));

    ImGui::PopID();
    if (hovered && tooltip && tooltip[0]) {
        ImGui::SetTooltip("%s", tooltip);
    }
    return clicked;
}

bool TextButton(const char* label, const ImVec2& size, bool active) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.34f, 0.44f, 0.72f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.44f, 0.54f, 0.82f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.54f, 0.64f, 0.92f, 1.00f));
    }
    bool r = ImGui::Button(label, size);
    if (active) ImGui::PopStyleColor(3);
    return r;
}

bool SeekBar(const char* id, float& value, float min, float max,
             float width, float height, bool* out_held) {
    ImGui::PushID(id);
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImVec2 size(width, height);

    ImGui::InvisibleButton("seek", size);
    bool held    = ImGui::IsItemActive();
    bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    float track_y = cursor.y + height * 0.5f;
    float track_h = (hovered || held) ? 7.0f : 5.0f;
    ImVec2 track_tl(cursor.x, track_y - track_h * 0.5f);
    ImVec2 track_br(cursor.x + width, track_y + track_h * 0.5f);
    dl->AddRectFilled(track_tl, track_br, IM_COL32(60, 62, 82, 200), track_h * 0.5f);

    float t = (max > min) ? (value - min) / (max - min) : 0.0f;
    if (t < 0) t = 0; if (t > 1) t = 1;
    float fill_x = cursor.x + width * t;

    // Filled portion with a subtle gradient
    dl->AddRectFilledMultiColor(track_tl, ImVec2(fill_x, track_br.y),
                                IM_COL32(120, 160, 240, 240),
                                IM_COL32(180, 205, 255, 240),
                                IM_COL32(180, 205, 255, 240),
                                IM_COL32(120, 160, 240, 240));

    // Hover indicator on the track
    if (hovered && !held) {
        float mx = ImGui::GetIO().MousePos.x;
        if (mx >= cursor.x && mx <= cursor.x + width) {
            dl->AddRectFilled(ImVec2(mx - 1, track_tl.y),
                              ImVec2(mx + 1, track_br.y),
                              IM_COL32(255, 255, 255, 120));
        }
    }

    // Thumb
    if (hovered || held) {
        float radius = held ? 9.0f : 7.5f;
        dl->AddCircleFilled(ImVec2(fill_x, track_y), radius + 3.0f,
                            IM_COL32(0, 0, 0, 120), 24);
        dl->AddCircleFilled(ImVec2(fill_x, track_y), radius,
                            IM_COL32(255, 255, 255, 255), 24);
        dl->AddCircle(ImVec2(fill_x, track_y), radius,
                      IM_COL32(130, 165, 240, 255), 24, 2.0f);
    }

    bool changed = false;
    if (held) {
        float mouse_x = ImGui::GetIO().MousePos.x;
        float new_t = (mouse_x - cursor.x) / width;
        if (new_t < 0) new_t = 0; if (new_t > 1) new_t = 1;
        float new_val = min + (max - min) * new_t;
        if (new_val != value) {
            value = new_val;
            changed = true;
        }
    }

    if (out_held) *out_held = held;
    ImGui::PopID();
    return changed;
}

void Badge(const char* text, ImU32 bg, ImU32 fg) {
    ImVec2 cursor = ImGui::GetCursorScreenPos();
    ImVec2 sz = ImGui::CalcTextSize(text);
    float pad_x = 8.0f;
    float pad_y = 3.0f;
    ImVec2 br(cursor.x + sz.x + pad_x * 2, cursor.y + sz.y + pad_y * 2);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(cursor, br, bg, (sz.y + pad_y * 2) * 0.5f);
    dl->AddText(ImVec2(cursor.x + pad_x, cursor.y + pad_y), fg, text);
    ImGui::Dummy(ImVec2(sz.x + pad_x * 2, sz.y + pad_y * 2));
}

} // namespace Style
