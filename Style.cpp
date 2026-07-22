#include "Style.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Style {

static constexpr float kPI = 3.14159265358979323846f;

// -----------------------------------------------------------------------------
// Palette. Two accents only: a cool sky-blue primary (reads clean on dark
// surfaces at all sizes and passes WCAG AA against #0E1116 base) and a warm
// amber secondary reserved for "off-nominal" states - AMP badge, warnings,
// B-marker on the loop bar. Kept as ImU32 so draw-list primitives can grab
// them without an ImVec4 round-trip.
// -----------------------------------------------------------------------------
namespace {
constexpr ImU32 kAccent      = IM_COL32( 96, 176, 246, 255); // sky blue
constexpr ImU32 kAccentLight = IM_COL32(148, 202, 255, 255); // hover / thumb glow
constexpr ImU32 kAccentDeep  = IM_COL32( 64, 132, 216, 255); // pressed / gradient toe
constexpr ImU32 kWarn        = IM_COL32(255, 168,  74, 255); // AMP + B marker
constexpr ImU32 kOK          = IM_COL32(128, 220, 148, 255); // A marker
constexpr ImU32 kTextHi      = IM_COL32(240, 244, 252, 255);
constexpr ImU32 kTextLo      = IM_COL32(170, 182, 205, 255);
} // namespace

void SetupDarkTheme() {
    ImGuiStyle& s = ImGui::GetStyle();

    s.WindowRounding    = 12.0f;
    s.ChildRounding     = 10.0f;
    s.FrameRounding     = 7.0f;
    s.PopupRounding     = 10.0f;
    s.ScrollbarRounding = 8.0f;
    s.GrabRounding      = 7.0f;
    s.TabRounding       = 6.0f;
    s.WindowBorderSize  = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.PopupBorderSize   = 1.0f;

    s.WindowPadding     = ImVec2(16, 14);
    s.FramePadding      = ImVec2(11, 7);
    s.ItemSpacing       = ImVec2(10, 8);
    s.ItemInnerSpacing  = ImVec2(6, 6);
    s.CellPadding       = ImVec2(6, 4);
    s.IndentSpacing     = 22.0f;
    s.ScrollbarSize     = 12.0f;
    s.GrabMinSize       = 12.0f;

    ImVec4* c = s.Colors;
    // Base neutrals shifted slightly cooler + a touch darker so the accent
    // pops harder. Text stays near-white for AA compliance.
    c[ImGuiCol_Text]                  = ImVec4(0.94f, 0.96f, 0.99f, 1.00f);
    c[ImGuiCol_TextDisabled]          = ImVec4(0.50f, 0.54f, 0.62f, 1.00f);
    c[ImGuiCol_WindowBg]              = ImVec4(0.05f, 0.06f, 0.08f, 0.96f);
    c[ImGuiCol_ChildBg]               = ImVec4(0.04f, 0.05f, 0.07f, 0.90f);
    c[ImGuiCol_PopupBg]               = ImVec4(0.07f, 0.08f, 0.11f, 0.98f);
    c[ImGuiCol_Border]                = ImVec4(0.22f, 0.26f, 0.34f, 0.55f);
    c[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_FrameBg]               = ImVec4(0.11f, 0.13f, 0.18f, 0.88f);
    c[ImGuiCol_FrameBgHovered]        = ImVec4(0.17f, 0.21f, 0.30f, 0.92f);
    c[ImGuiCol_FrameBgActive]         = ImVec4(0.23f, 0.30f, 0.44f, 0.96f);
    c[ImGuiCol_TitleBg]               = ImVec4(0.05f, 0.06f, 0.08f, 1.00f);
    c[ImGuiCol_TitleBgActive]         = ImVec4(0.09f, 0.11f, 0.15f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.05f, 0.06f, 0.08f, 0.75f);
    c[ImGuiCol_MenuBarBg]             = ImVec4(0.07f, 0.08f, 0.11f, 0.98f);
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0.02f, 0.03f, 0.05f, 0.45f);
    c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.28f, 0.34f, 0.46f, 0.75f);
    c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.40f, 0.50f, 0.72f, 0.90f);
    c[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.55f, 0.72f, 0.95f, 1.00f);
    c[ImGuiCol_CheckMark]             = ImVec4(0.55f, 0.75f, 1.00f, 1.00f);
    c[ImGuiCol_SliderGrab]            = ImVec4(0.55f, 0.75f, 1.00f, 1.00f);
    c[ImGuiCol_SliderGrabActive]      = ImVec4(0.75f, 0.90f, 1.00f, 1.00f);
    c[ImGuiCol_Button]                = ImVec4(0.13f, 0.16f, 0.22f, 0.90f);
    c[ImGuiCol_ButtonHovered]         = ImVec4(0.23f, 0.32f, 0.50f, 0.95f);
    c[ImGuiCol_ButtonActive]          = ImVec4(0.34f, 0.50f, 0.78f, 1.00f);
    c[ImGuiCol_Header]                = ImVec4(0.16f, 0.22f, 0.34f, 0.80f);
    c[ImGuiCol_HeaderHovered]         = ImVec4(0.24f, 0.34f, 0.54f, 0.90f);
    c[ImGuiCol_HeaderActive]          = ImVec4(0.34f, 0.50f, 0.78f, 1.00f);
    c[ImGuiCol_Separator]             = ImVec4(0.22f, 0.26f, 0.34f, 0.55f);
    c[ImGuiCol_SeparatorHovered]      = ImVec4(0.34f, 0.50f, 0.78f, 0.78f);
    c[ImGuiCol_SeparatorActive]       = ImVec4(0.55f, 0.72f, 0.95f, 1.00f);
    c[ImGuiCol_ResizeGrip]            = ImVec4(0.22f, 0.28f, 0.40f, 0.30f);
    c[ImGuiCol_ResizeGripHovered]     = ImVec4(0.34f, 0.50f, 0.78f, 0.78f);
    c[ImGuiCol_ResizeGripActive]      = ImVec4(0.55f, 0.72f, 0.95f, 1.00f);
    c[ImGuiCol_Tab]                   = ImVec4(0.11f, 0.14f, 0.20f, 0.86f);
    c[ImGuiCol_TabHovered]            = ImVec4(0.28f, 0.40f, 0.60f, 0.86f);
    c[ImGuiCol_PlotLines]             = ImVec4(0.65f, 0.78f, 1.00f, 1.00f);
    c[ImGuiCol_PlotLinesHovered]      = ImVec4(0.86f, 0.92f, 1.00f, 1.00f);
    c[ImGuiCol_PlotHistogram]         = ImVec4(0.55f, 0.75f, 1.00f, 1.00f);
    c[ImGuiCol_PlotHistogramHovered]  = ImVec4(0.75f, 0.90f, 1.00f, 1.00f);
    c[ImGuiCol_TextSelectedBg]        = ImVec4(0.30f, 0.48f, 0.80f, 0.55f);
    c[ImGuiCol_DragDropTarget]        = ImVec4(0.75f, 0.90f, 1.00f, 0.90f);
    c[ImGuiCol_NavCursor]             = ImVec4(0.55f, 0.75f, 1.00f, 1.00f);
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.75f);
    c[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.80f, 0.80f, 0.80f, 0.24f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.02f, 0.03f, 0.05f, 0.60f);
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
    // Slightly smaller outer, slightly bigger inner so the teeth read as
    // "cog with distinct teeth" rather than a pinwheel. 6 teeth is easier to
    // parse at small icon sizes than 8 (matches the icons in Figma / macOS
    // sidebar patterns).
    const int teeth = 6;
    const float outer = s * 0.98f;
    const float inner = s * 0.72f;
    const float hub   = s * 0.28f;
    for (int i = 0; i < teeth * 2; ++i) {
        float a0 = (float)i * kPI / teeth;
        float a1 = (float)(i + 1) * kPI / teeth;
        float r0 = (i % 2 == 0) ? outer : inner;
        float r1 = (i % 2 == 0) ? outer : inner;
        ImVec2 p0(c.x + std::cos(a0) * r0, c.y + std::sin(a0) * r0);
        ImVec2 p1(c.x + std::cos(a1) * r1, c.y + std::sin(a1) * r1);
        dl->AddTriangleFilled(c, p0, p1, col);
    }
    // Hub: an accent-coloured dot in the middle so the settings icon has a
    // recognisable focal point instead of just being "circle in a circle."
    // We can't subtract to make a real hole, so pick a colour that reads on
    // most surfaces: the accent's darker toe.
    dl->AddCircleFilled(c, hub, IM_COL32(45, 78, 130, 255), 28);
    dl->AddCircle(c, hub, col, 28, std::max(1.5f, s * 0.14f));
}

void DrawIcon(ImDrawList* dl, ImVec2 c, float s, Icon icon, ImU32 col) {
    const float t = std::max(1.6f, s * 0.16f);

    switch (icon) {
    case Icon::Play: {
        // Filled triangle nudged slightly right so the optical center sits at
        // `c` (a sharp isoceles triangle's centroid is 1/3 from the base -
        // without the offset the icon looks left-shifted inside a square).
        ImVec2 p1(c.x - s * 0.32f, c.y - s * 0.62f);
        ImVec2 p2(c.x - s * 0.32f, c.y + s * 0.62f);
        ImVec2 p3(c.x + s * 0.68f, c.y);
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
        // Camera silhouette. The lens "cutout" used to be filled with a
        // hardcoded near-black which broke when the theme base shifted;
        // now we invert the drawing order: draw the body as a ring so the
        // window background shows through the lens naturally.
        dl->AddRectFilled(ImVec2(c.x - s * 0.9f, c.y - s * 0.55f),
                          ImVec2(c.x + s * 0.9f, c.y + s * 0.65f), col, s * 0.15f);
        // Viewfinder bump - shifted symmetrically so the icon reads centered.
        dl->AddRectFilled(ImVec2(c.x - s * 0.28f, c.y - s * 0.80f),
                          ImVec2(c.x + s * 0.28f, c.y - s * 0.55f), col, s * 0.10f);
        // Two-ring lens. Since ImGui draw lists don't blend-subtract we can't
        // literally cut a hole; the two-ring stroke + inner dot look reads
        // as a lens without needing a fake-background cutout.
        dl->AddCircle(c, s * 0.36f, col, 32, t * 0.9f);
        dl->AddCircleFilled(c, s * 0.18f, col, 24);
        // Tiny flash bulb top-right so the icon actually looks like a camera
        // rather than a chunky rectangle with a target.
        dl->AddCircleFilled(ImVec2(c.x + s * 0.62f, c.y - s * 0.30f),
                            s * 0.09f, col, 12);
        break;
    }
    case Icon::Loop: {
        // Circular arrow. The arc sweeps ~230 deg leaving a clean gap at the
        // top-right; the arrowhead is aligned tangent to the arc so the tip
        // reads as motion instead of a random spike.
        const float radius = s * 0.68f;
        const float start  = -kPI * 0.15f;   // arc starts at ~ -27 deg
        const float end    =  kPI * 1.15f;   // sweeps around clockwise
        dl->PathArcTo(c, radius, start, end, 40);
        dl->PathStroke(col, 0, t);
        // Arrow head: an equilateral triangle whose base sits on the arc
        // tangent at the start angle. Points +tangent (motion direction).
        float ang = start;
        ImVec2 tip(c.x + std::cos(ang) * (radius + s * 0.10f),
                   c.y + std::sin(ang) * (radius + s * 0.10f));
        float side = s * 0.32f;
        // Two base vertices perpendicular to the tangent, offset back along it.
        ImVec2 tang(std::cos(ang + kPI * 0.5f), std::sin(ang + kPI * 0.5f));
        ImVec2 back(-std::cos(ang), -std::sin(ang));
        ImVec2 p2(tip.x + back.x * side + tang.x * side * 0.55f,
                  tip.y + back.y * side + tang.y * side * 0.55f);
        ImVec2 p3(tip.x + back.x * side - tang.x * side * 0.55f,
                  tip.y + back.y * side - tang.y * side * 0.55f);
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
        // Three rows: two "queued" items (bullet + line) and a bottom row
        // with a small play triangle to signal "this is a playlist / queue"
        // rather than a generic list. Bullets are slightly bigger for a
        // cleaner read at small icon sizes.
        for (int i = 0; i < 2; ++i) {
            float y = c.y + (i - 1) * s * 0.40f;
            dl->AddCircleFilled(ImVec2(c.x - s * 0.68f, y), s * 0.12f, col, 16);
            dl->AddRectFilled(ImVec2(c.x - s * 0.42f, y - s * 0.07f),
                              ImVec2(c.x + s * 0.82f, y + s * 0.07f), col, s * 0.07f);
        }
        // Bottom "now playing" row: play triangle + shorter bar.
        float ny = c.y + s * 0.40f;
        ImVec2 t1(c.x - s * 0.80f, ny - s * 0.20f);
        ImVec2 t2(c.x - s * 0.80f, ny + s * 0.20f);
        ImVec2 t3(c.x - s * 0.48f, ny);
        dl->AddTriangleFilled(t1, t2, t3, col);
        dl->AddRectFilled(ImVec2(c.x - s * 0.32f, ny - s * 0.07f),
                          ImVec2(c.x + s * 0.60f, ny + s * 0.07f), col, s * 0.07f);
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
        // "i" in a circle. Bigger dot, taller stem so it reads at 12 px sizes
        // and doesn't look like a period stuck to a rectangle.
        dl->AddCircle(c, s * 0.88f, col, 36, t * 0.95f);
        dl->AddCircleFilled(ImVec2(c.x, c.y - s * 0.42f), s * 0.14f, col, 16);
        dl->AddRectFilled(ImVec2(c.x - s * 0.11f, c.y - s * 0.15f),
                          ImVec2(c.x + s * 0.11f, c.y + s * 0.55f), col, s * 0.08f);
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

    // Background states:
    //  - active: solid accent (currently-selected mode; e.g. Playlist open)
    //  - held:   accent-deep, slightly translucent (finger-on-glass)
    //  - hover:  desaturated neutral so hover feels informational, not "on"
    ImU32 bg = IM_COL32(0, 0, 0, 0);
    if (active)       bg = IM_COL32( 96, 176, 246, 225);
    else if (held)    bg = IM_COL32( 64, 132, 216, 210);
    else if (hovered) bg = IM_COL32(255, 255, 255,  30);
    bg = Fade(bg, alpha);
    if ((bg >> IM_COL32_A_SHIFT) != 0) {
        float rounding = std::min(size.x, size.y) * 0.30f;
        dl->AddRectFilled(cursor, br, bg, rounding);
        // Subtle inset highlight on the top edge of the active state to add
        // dimensionality without introducing a full material shadow.
        if (active) {
            dl->AddLine(ImVec2(cursor.x + rounding, cursor.y + 0.5f),
                        ImVec2(br.x - rounding,     cursor.y + 0.5f),
                        Fade(IM_COL32(255, 255, 255, 40), alpha), 1.0f);
        }
    }

    ImVec2 center(cursor.x + size.x * 0.5f, cursor.y + size.y * 0.5f);
    float icon_size = std::min(size.x, size.y) * 0.38f * scale;
    // Foreground: near-white when active or hovered so the icon reads clean
    // against the accent fill; slightly muted when at rest so the row of
    // icons doesn't visually shout.
    ImU32 fg;
    if (active)      fg = IM_COL32(255, 255, 255, 255);
    else if (hovered || held) fg = IM_COL32(242, 246, 252, 255);
    else             fg = IM_COL32(216, 222, 236, 255);
    DrawIcon(dl, center, icon_size, icon, Fade(fg, alpha));

    ImGui::PopID();
    if (hovered && tooltip && tooltip[0]) {
        ImGui::SetTooltip("%s", tooltip);
    }
    return clicked;
}

bool TextButton(const char* label, const ImVec2& size, bool active) {
    if (active) {
        // Solid accent for the primary CTA (splash "Open File...", playlist
        // "Clear" when armed). Uses the same sky-blue accent as SeekBar +
        // active IconButtons for palette coherence.
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.30f, 0.55f, 0.86f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.40f, 0.66f, 0.96f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.24f, 0.44f, 0.72f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1.00f, 1.00f, 1.00f, 1.00f));
    }
    bool r = ImGui::Button(label, size);
    if (active) ImGui::PopStyleColor(4);
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
    // Idle rest thickness stays low so the bar reads as "at rest" when the
    // cursor isn't near it (matches YouTube / Netflix rhythm); a bigger step
    // on hover / active state makes the affordance unmistakable.
    float track_h = (held ? 9.0f : (hovered ? 8.0f : 4.0f));
    float radius  = track_h * 0.5f;
    ImVec2 track_tl(cursor.x, track_y - track_h * 0.5f);
    ImVec2 track_br(cursor.x + width, track_y + track_h * 0.5f);
    // Rest-track: slightly lighter to keep visibility over the darkest video
    // frames while still being obviously non-progress.
    dl->AddRectFilled(track_tl, track_br, IM_COL32(78, 86, 108, 200), radius);

    float t = (max > min) ? (value - min) / (max - min) : 0.0f;
    if (t < 0) t = 0; if (t > 1) t = 1;
    float fill_x = cursor.x + width * t;

    // Filled portion: horizontal gradient from the deep accent (toe) into the
    // primary and finally the light hover accent at the head so the eye is
    // pulled toward the current position - reads as motion even when the
    // clip is paused.
    dl->AddRectFilledMultiColor(track_tl, ImVec2(fill_x, track_br.y),
                                kAccentDeep, kAccentLight,
                                kAccentLight, kAccentDeep);

    // Hover time-indicator on the track: a soft column of light where the
    // cursor is. Wider and more visible than the previous 1 px line.
    if (hovered && !held) {
        float mx = ImGui::GetIO().MousePos.x;
        if (mx >= cursor.x && mx <= cursor.x + width) {
            dl->AddRectFilled(ImVec2(mx - 1.5f, track_tl.y - 1.0f),
                              ImVec2(mx + 1.5f, track_br.y + 1.0f),
                              IM_COL32(255, 255, 255, 160), radius);
        }
    }

    // Thumb: soft outer glow -> shadow ring -> white core -> accent ring.
    // Visible any time the bar is hovered OR the user is actively dragging.
    if (hovered || held) {
        float r = held ? 8.5f : 7.5f;
        // Soft outer glow (two blurred-looking circles at reducing alpha).
        dl->AddCircleFilled(ImVec2(fill_x, track_y), r + 6.0f,
                            IM_COL32(96, 176, 246, 55), 32);
        dl->AddCircleFilled(ImVec2(fill_x, track_y), r + 3.5f,
                            IM_COL32(96, 176, 246, 105), 32);
        // Contact shadow so the thumb feels physically lifted off the track.
        dl->AddCircleFilled(ImVec2(fill_x, track_y + 1.0f), r + 1.5f,
                            IM_COL32(0, 0, 0, 130), 32);
        // Solid white core.
        dl->AddCircleFilled(ImVec2(fill_x, track_y), r,
                            IM_COL32(255, 255, 255, 255), 32);
        // Accent ring pulled tighter than the old 2 px stroke so the white
        // core dominates - matches YouTube / Netflix ratio.
        dl->AddCircle(ImVec2(fill_x, track_y), r,
                      kAccent, 32, 1.5f);
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
    float pad_x = 9.0f;
    float pad_y = 3.5f;
    ImVec2 br(cursor.x + sz.x + pad_x * 2, cursor.y + sz.y + pad_y * 2);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float rounding = (sz.y + pad_y * 2) * 0.5f;
    dl->AddRectFilled(cursor, br, bg, rounding);
    // Hairline highlight along the top of the pill so it reads as a
    // "physical" pill rather than a flat coloured rectangle - the same
    // trick every modern OS uses on tab bars / segmented controls.
    int a = (bg >> IM_COL32_A_SHIFT) & 0xFF;
    ImU32 hi = IM_COL32(255, 255, 255, std::min(80, a / 3));
    dl->AddLine(ImVec2(cursor.x + rounding * 0.6f, cursor.y + 0.5f),
                ImVec2(br.x     - rounding * 0.6f, cursor.y + 0.5f),
                hi, 1.0f);
    dl->AddText(ImVec2(cursor.x + pad_x, cursor.y + pad_y), fg, text);
    ImGui::Dummy(ImVec2(sz.x + pad_x * 2, sz.y + pad_y * 2));
}

} // namespace Style
