// VAR HUD renderer: visual layer of the VAR panel (ImGui draw lists over the native Fox Engine Present).
// Interaction semantics live in VarHudController (var_hud.cpp). This file only draws and routes input.
//
// Visual system
//   * One accent colour (blue). Yellow / red / green / orange only carry meaning (cards, penalty, ready).
//   * Spacing on a 4 px grid, radii 12 / 8 / 6 / 4, everything multiplied by the UI scale S.
//   * Five text styles (body, strong, title, small, caption) rasterized at the real UI scale of the current
//     resolution, so text stays crisp from 720p to 4K (no bitmap stretching).
//   * Prompt glyphs (keycaps / gamepad buttons) follow the user's bindings and the last device used.

#include "var_hud.h"

#include "i18n.h"
#include "imgui.h"
#include "input_manager.h"
#include "update_checker.h"
#include "version.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace var_mod {

namespace {

VarHudController g_controller;
std::atomic<bool> g_resetPending{ false };
// VarState data revision captured this frame (render thread only). Keys per-frame caches so a new
// snapshot, incident, verdict or match reset always invalidates them.
uint32_t g_frameDataRevision = 0;
uint32_t g_frameSnapRevision = 0;
// Horizontal extent of the open panel in the last frame it was drawn: banners and toasts are centred in the free
// part of the screen so they never cover the panel on small or narrow screens
float    g_panelX0 = 0.0f;
float    g_panelX1 = 0.0f;
uint64_t g_panelDrawnFrame = 0;
uint64_t g_hudFrame = 0;

// X of an element of width w: centred on the screen, or in the free side when the open panel would cover it
float CenterOutsidePanel(float vw, float w) {
    const float centered = std::floor((vw - w) * 0.5f);
    if (g_panelDrawnFrame + 1 < g_hudFrame || g_panelX1 <= g_panelX0) return centered;
    if (centered + w <= g_panelX0 || centered >= g_panelX1) return centered;
    const float leftFree = g_panelX0;
    const float rightFree = vw - g_panelX1;
    const float gap = 12.0f;
    if (leftFree >= rightFree && w + 2.0f * gap <= leftFree) return std::floor((leftFree - w) * 0.5f);
    if (rightFree > leftFree && w + 2.0f * gap <= rightFree) return std::floor(g_panelX1 + (rightFree - w) * 0.5f);
    return centered; // no free side wide enough: keep it centred
}
HudPage g_pageAtFrameStart = HudPage::Main; // page before this frame's mouse clicks
uint64_t g_frameNamesRevision = 0; // roster + teams revisions of this frame (names in summaries)
bool g_scrollToSelection = false;
int g_lastScalePreset = 1;

// Current UI scale for this frame (== font bake scale when fonts are built)
float S = 1.0f;

inline float U(float v) { return std::floor(v * S + 0.5f); }
inline float Uf(float v) { return v * S; }
inline ImVec2 Snap(ImVec2 p) { return ImVec2(std::floor(p.x + 0.5f), std::floor(p.y + 0.5f)); }

// ============================================================================
// Tokens
// ============================================================================
namespace C {
constexpr ImU32 Panel      = IM_COL32(12, 15, 21, 244);
constexpr ImU32 PanelSolid = IM_COL32(12, 15, 21, 255);
constexpr ImU32 Surface    = IM_COL32(22, 27, 36, 255);
constexpr ImU32 SurfaceHi  = IM_COL32(30, 36, 48, 255);
constexpr ImU32 Selected   = IM_COL32(28, 44, 70, 255);
constexpr ImU32 Hair       = IM_COL32(255, 255, 255, 18);
constexpr ImU32 Stroke     = IM_COL32(255, 255, 255, 30);
constexpr ImU32 StrokeHi   = IM_COL32(255, 255, 255, 60);
constexpr ImU32 Text       = IM_COL32(238, 242, 247, 255);
constexpr ImU32 Text2      = IM_COL32(160, 170, 186, 255);
constexpr ImU32 Text3      = IM_COL32(108, 118, 136, 255);
constexpr ImU32 Accent     = IM_COL32(56, 150, 255, 255);
constexpr ImU32 AccentHi   = IM_COL32(88, 170, 255, 255);
constexpr ImU32 AccentSoft = IM_COL32(56, 150, 255, 44);
constexpr ImU32 Yellow     = IM_COL32(246, 196, 46, 255);
constexpr ImU32 Red        = IM_COL32(236, 76, 84, 255);
constexpr ImU32 Green      = IM_COL32(52, 199, 112, 255);
constexpr ImU32 Orange     = IM_COL32(248, 150, 64, 255);
constexpr ImU32 Suspect    = IM_COL32(255, 214, 102, 255);
// Roles of the play: away from the two colours that misled (red = a common kit, blue = the UI focus colour)
constexpr ImU32 RoleOffender = IM_COL32(255, 138, 48, 255);
constexpr ImU32 RoleFouled   = IM_COL32(64, 214, 232, 255);
constexpr ImU32 Neutral    = IM_COL32(176, 186, 202, 255);
// Pitch
// Tactical pitch: dark, cool and low-saturation, so team colours (including green kits) and numbers pop
constexpr ImU32 Surround   = IM_COL32(10, 18, 16, 255);
constexpr ImU32 GrassA     = IM_COL32(20, 46, 36, 255);
constexpr ImU32 GrassB     = IM_COL32(23, 52, 40, 255);
constexpr ImU32 Line       = IM_COL32(220, 236, 228, 92);
constexpr ImU32 Offside    = IM_COL32(130, 196, 255, 150);
constexpr ImU32 OffsideText = IM_COL32(150, 206, 255, 255);
} // namespace C

constexpr float kRadPanel = 12.0f;
constexpr float kRadCard  = 8.0f;
constexpr float kRadCtl   = 6.0f;
constexpr float kRadChip  = 4.0f;

ImU32 WithAlpha(ImU32 c, float a) {
    const uint32_t alpha = static_cast<uint32_t>(std::clamp(a, 0.0f, 1.0f) * ((c >> IM_COL32_A_SHIFT) & 0xFF));
    return (c & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
}

ImU32 RgbToImU32(RgbColor c, uint8_t alpha = 255) { return IM_COL32(c.r, c.g, c.b, alpha); }

// Coloured left edge of a card or row: the container's own rounded left side clipped to the bar width, so the
// bar runs the full height and bends exactly with the container's corners
void EdgeAccent(ImDrawList* dl, ImVec2 p, ImVec2 e, ImU32 col, float radius, float width) {
    dl->PushClipRect(p, ImVec2(p.x + width, e.y), true);
    dl->AddRectFilled(p, e, col, radius, ImDrawFlags_RoundCornersLeft);
    dl->PopClipRect();
}

ImU32 ContrastTextForRgb(RgbColor c) {
    return (ColorHelpers::RelativeLuma(c) > 152.0f) ? IM_COL32(14, 20, 30, 255) : IM_COL32(246, 250, 255, 255);
}

bool IsNonZeroVec(const Vec3& v) { return (v.x * v.x + v.z * v.z) > 0.01f; }

// ============================================================================
// Fonts & text
// ============================================================================
struct FontSet {
    ImFont* body    = nullptr; // Segoe UI 16.5
    ImFont* strong  = nullptr; // Segoe UI Semibold 16.5
    ImFont* title   = nullptr; // Segoe UI Bold 20
    ImFont* sm      = nullptr; // Segoe UI 14
    ImFont* caption = nullptr; // Segoe UI Semibold 12.5 (uppercase, tracked)
    float   baked   = 0.0f;
};
FontSet F;

enum class Txt { Body, Strong, Title, Small, Caption };

struct Face {
    ImFont* font;
    float   size;
};

Face FaceOf(Txt t) {
    ImFont* f = nullptr;
    float logical = 16.5f;
    switch (t) {
        case Txt::Body:    f = F.body;    logical = 16.5f; break;
        case Txt::Strong:  f = F.strong;  logical = 16.5f; break;
        case Txt::Title:   f = F.title;   logical = 20.0f; break;
        case Txt::Small:   f = F.sm;   logical = 14.0f; break;
        case Txt::Caption: f = F.caption; logical = 12.5f; break;
    }
    if (f) return { f, f->FontSize };
    return { ImGui::GetFont(), logical * S };
}

float LineH(Txt t) { return FaceOf(t).size; }

int Utf8Len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c >> 5) == 0x6) return 2;
    if ((c >> 4) == 0xE) return 3;
    if ((c >> 3) == 0x1E) return 4;
    return 1;
}

// Length of the character at q, never stepping over the terminator of a truncated sequence
int Utf8LenAt(const char* q) {
    const int n = Utf8Len(static_cast<unsigned char>(*q));
    for (int i = 1; i < n; ++i) {
        if (q[i] == '\0') return i;
    }
    return n;
}

float Measure(Txt t, const char* s, const char* e = nullptr) {
    const Face fc = FaceOf(t);
    return fc.font->CalcTextSizeA(fc.size, FLT_MAX, 0.0f, s, e).x;
}

void Text(ImDrawList* dl, Txt t, ImVec2 p, ImU32 col, const char* s, const char* e = nullptr) {
    const Face fc = FaceOf(t);
    dl->AddText(fc.font, fc.size, Snap(p), col, s, e);
}

// Draws text clipped to maxW with a trailing ellipsis. Returns the drawn width.
float TextFit(ImDrawList* dl, Txt t, ImVec2 p, float maxW, ImU32 col, const char* s) {
    const Face fc = FaceOf(t);
    const float full = fc.font->CalcTextSizeA(fc.size, FLT_MAX, 0.0f, s).x;
    if (full <= maxW) {
        dl->AddText(fc.font, fc.size, Snap(p), col, s);
        return full;
    }
    static const char* kEll = "\xE2\x80\xA6";
    const float ellW = fc.font->CalcTextSizeA(fc.size, FLT_MAX, 0.0f, kEll).x;
    const char* end = s;
    float w = 0.0f;
    for (const char* q = s; *q;) {
        const int n = Utf8LenAt(q);
        const float cw = fc.font->CalcTextSizeA(fc.size, FLT_MAX, 0.0f, q, q + n).x;
        if (w + cw + ellW > maxW) break;
        w += cw;
        q += n;
        end = q;
    }
    while (end > s && end[-1] == ' ') --end;
    w = fc.font->CalcTextSizeA(fc.size, FLT_MAX, 0.0f, s, end).x;
    dl->AddText(fc.font, fc.size, Snap(p), col, s, end);
    dl->AddText(fc.font, fc.size, Snap(ImVec2(p.x + w, p.y)), col, kEll);
    return w + ellW;
}

// Below this size the extra letter spacing is under a pixel and per-glyph pixel snapping makes the gaps uneven
// ("INFRA TOR"): small captions are drawn as one run with the font's own spacing instead
constexpr float kMinTrackedFontPx = 11.0f;

// Uppercase caption with letter spacing (broadcast style). draw=false only measures.
float Tracked(ImDrawList* dl, ImVec2 p, ImU32 col, const char* s, bool draw = true, Txt t = Txt::Caption) {
    const Face fc = FaceOf(t);
    if (fc.size < kMinTrackedFontPx) {
        if (draw && *s) dl->AddText(fc.font, fc.size, Snap(p), col, s);
        return *s ? fc.font->CalcTextSizeA(fc.size, FLT_MAX, 0.0f, s).x : 0.0f;
    }
    const float track = Uf(0.9f);
    float x = p.x;
    bool any = false;
    for (const char* q = s; *q;) {
        const int n = Utf8LenAt(q);
        if (draw) dl->AddText(fc.font, fc.size, Snap(ImVec2(x, p.y)), col, q, q + n);
        x += fc.font->CalcTextSizeA(fc.size, FLT_MAX, 0.0f, q, q + n).x + track;
        q += n;
        any = true;
    }
    return any ? (x - p.x - track) : 0.0f;
}

float TrackedW(const char* s) { return Tracked(nullptr, ImVec2(0, 0), 0, s, false); }

// Tracked caption limited to maxW with a trailing ellipsis. Returns the drawn width.
float TrackedFit(ImDrawList* dl, ImVec2 p, ImU32 col, const char* s, float maxW, Txt t = Txt::Caption) {
    if (Tracked(nullptr, p, col, s, false, t) <= maxW) return Tracked(dl, p, col, s, true, t);
    const Face fc = FaceOf(t);
    if (fc.size < kMinTrackedFontPx) return TextFit(dl, t, p, maxW, col, s); // one run, font spacing
    const float track = Uf(0.9f);
    static const char* kEll = "\xE2\x80\xA6";
    const float ellW = fc.font->CalcTextSizeA(fc.size, FLT_MAX, 0.0f, kEll).x;
    float x = p.x;
    for (const char* q = s; *q;) {
        const int n = Utf8LenAt(q);
        const float cw = fc.font->CalcTextSizeA(fc.size, FLT_MAX, 0.0f, q, q + n).x;
        if (x - p.x + cw + track + ellW > maxW) break;
        dl->AddText(fc.font, fc.size, Snap(ImVec2(x, p.y)), col, q, q + n);
        x += cw + track;
        q += n;
    }
    dl->AddText(fc.font, fc.size, Snap(ImVec2(x, p.y)), col, kEll);
    return x - p.x + ellW;
}

// PT-BR uses decimal comma
void FormatMeters(char* out, size_t n, float v) {
    std::snprintf(out, n, "%.1f m", v);
    if (i18n::GetLanguage() == Language::PtBr) {
        for (char* c = out; *c; ++c) if (*c == '.') *c = ',';
    }
}

// ============================================================================
// Icons (vector, colour-agnostic)
// ============================================================================
enum class Dir { Left, Right, Up, Down };

void IconChevron(ImDrawList* dl, ImVec2 c, float sz, Dir d, ImU32 col) {
    const float h = sz * 0.5f, w = sz * 0.28f;
    const float th = std::max(1.5f, Uf(1.7f));
    ImVec2 pts[3];
    switch (d) {
        case Dir::Left:  pts[0] = ImVec2(c.x + w, c.y - h); pts[1] = ImVec2(c.x - w, c.y); pts[2] = ImVec2(c.x + w, c.y + h); break;
        case Dir::Right: pts[0] = ImVec2(c.x - w, c.y - h); pts[1] = ImVec2(c.x + w, c.y); pts[2] = ImVec2(c.x - w, c.y + h); break;
        case Dir::Up:    pts[0] = ImVec2(c.x - h, c.y + w); pts[1] = ImVec2(c.x, c.y - w); pts[2] = ImVec2(c.x + h, c.y + w); break;
        case Dir::Down:  pts[0] = ImVec2(c.x - h, c.y - w); pts[1] = ImVec2(c.x, c.y + w); pts[2] = ImVec2(c.x + h, c.y - w); break;
    }
    dl->AddPolyline(pts, 3, col, ImDrawFlags_None, th);
}

void IconTriangle(ImDrawList* dl, ImVec2 c, float sz, Dir d, ImU32 col) {
    const float h = sz * 0.5f;
    switch (d) {
        case Dir::Up:    dl->AddTriangleFilled(ImVec2(c.x, c.y - h * 0.8f), ImVec2(c.x + h, c.y + h * 0.6f), ImVec2(c.x - h, c.y + h * 0.6f), col); break;
        case Dir::Down:  dl->AddTriangleFilled(ImVec2(c.x - h, c.y - h * 0.6f), ImVec2(c.x + h, c.y - h * 0.6f), ImVec2(c.x, c.y + h * 0.8f), col); break;
        case Dir::Left:  dl->AddTriangleFilled(ImVec2(c.x - h * 0.8f, c.y), ImVec2(c.x + h * 0.6f, c.y - h), ImVec2(c.x + h * 0.6f, c.y + h), col); break;
        case Dir::Right: dl->AddTriangleFilled(ImVec2(c.x + h * 0.8f, c.y), ImVec2(c.x - h * 0.6f, c.y + h), ImVec2(c.x - h * 0.6f, c.y - h), col); break;
    }
}

void IconPlay(ImDrawList* dl, ImVec2 c, float sz, ImU32 col) {
    const float h = sz * 0.5f;
    dl->AddTriangleFilled(ImVec2(c.x - h * 0.55f, c.y - h), ImVec2(c.x + h * 0.85f, c.y), ImVec2(c.x - h * 0.55f, c.y + h), col);
}

void IconPause(ImDrawList* dl, ImVec2 c, float sz, ImU32 col) {
    const float h = sz * 0.5f, bw = sz * 0.22f, gap = sz * 0.14f;
    dl->AddRectFilled(ImVec2(c.x - gap - bw, c.y - h), ImVec2(c.x - gap, c.y + h), col, Uf(1.0f));
    dl->AddRectFilled(ImVec2(c.x + gap, c.y - h), ImVec2(c.x + gap + bw, c.y + h), col, Uf(1.0f));
}

void IconMinimize(ImDrawList* dl, ImVec2 c, float sz, ImU32 col) {
    const float h = sz * 0.45f;
    dl->AddLine(ImVec2(c.x - h, c.y), ImVec2(c.x + h, c.y), col, std::max(1.5f, Uf(1.8f)));
}

void IconClose(ImDrawList* dl, ImVec2 c, float sz, ImU32 col) {
    const float h = sz * 0.40f;
    const float th = std::max(1.4f, Uf(1.6f));
    dl->AddLine(ImVec2(c.x - h, c.y - h), ImVec2(c.x + h, c.y + h), col, th);
    dl->AddLine(ImVec2(c.x + h, c.y - h), ImVec2(c.x - h, c.y + h), col, th);
}

void IconSwap(ImDrawList* dl, ImVec2 c, float sz, ImU32 col) {
    const float w = sz * 0.5f, dy = sz * 0.22f, head = sz * 0.2f;
    const float th = std::max(1.4f, Uf(1.6f));
    dl->AddLine(ImVec2(c.x - w, c.y - dy), ImVec2(c.x + w, c.y - dy), col, th);
    dl->AddLine(ImVec2(c.x + w, c.y - dy), ImVec2(c.x + w - head, c.y - dy - head), col, th);
    dl->AddLine(ImVec2(c.x + w, c.y + dy), ImVec2(c.x - w, c.y + dy), col, th);
    dl->AddLine(ImVec2(c.x - w, c.y + dy), ImVec2(c.x - w + head, c.y + dy + head), col, th);
}

void IconCamera(ImDrawList* dl, ImVec2 c, float sz, ImU32 col) {
    const float w = sz * 0.5f, h = sz * 0.34f;
    const float th = std::max(1.4f, Uf(1.5f));
    dl->AddRect(ImVec2(c.x - w, c.y - h + sz * 0.06f), ImVec2(c.x + w * 0.45f, c.y + h), col, Uf(2.0f), 0, th);
    dl->AddTriangleFilled(ImVec2(c.x + w * 0.5f, c.y), ImVec2(c.x + w, c.y - h * 0.75f), ImVec2(c.x + w, c.y + h * 0.75f), col);
}

void IconReset(ImDrawList* dl, ImVec2 c, float sz, ImU32 col) {
    const float r = sz * 0.42f;
    const float th = std::max(1.4f, Uf(1.6f));
    dl->PathArcTo(c, r, 0.9f, 5.4f, 18);
    dl->PathStroke(col, ImDrawFlags_None, th);
    const ImVec2 tip(c.x + r * std::cos(0.9f), c.y + r * std::sin(0.9f));
    dl->AddTriangleFilled(ImVec2(tip.x + sz * 0.16f, tip.y - sz * 0.02f), ImVec2(tip.x - sz * 0.1f, tip.y - sz * 0.06f),
                          ImVec2(tip.x + sz * 0.02f, tip.y + sz * 0.2f), col);
}

void DrawDashedLine(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float thickness, float dashLen, float gapLen) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float dist = std::sqrt(dx * dx + dy * dy);
    // Positions come from game memory: a non-finite or absurd value must never turn this into an endless loop
    if (!std::isfinite(dist) || dist < 1.0f || dist > 20000.0f || dashLen + gapLen < 0.5f) return;
    const float ux = dx / dist, uy = dy / dist;
    for (float t = 0.0f; t < dist; t += dashLen + gapLen) {
        const float te = std::min(t + dashLen, dist);
        dl->AddLine(ImVec2(a.x + ux * t, a.y + uy * t), ImVec2(a.x + ux * te, a.y + uy * te), col, thickness);
    }
}

void VarBadge(ImDrawList* dl, ImVec2 p, float h, float* outW = nullptr) {
    const float tw = Measure(Txt::Strong, "VAR");
    const float w = tw + U(14);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), C::Accent, U(5));
    Text(dl, Txt::Strong, ImVec2(p.x + (w - tw) * 0.5f, p.y + (h - LineH(Txt::Strong)) * 0.5f - Uf(0.5f)), IM_COL32(255, 255, 255, 255), "VAR");
    if (outW) *outW = w;
}

// ============================================================================
// Prompt glyphs (keycaps / gamepad buttons)
// ============================================================================
bool IsArrowVk(uint8_t vk) { return vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT; }

float Keycap(ImDrawList* dl, ImVec2 p, float h, uint8_t vk, ImU32 textCol = C::Text2, bool draw = true,
             float maxW = 0.0f) {
    if (vk == 0) return 0.0f;
    const float pad = U(6);
    char name[32];
    float w = h;
    if (!IsArrowVk(vk)) {
        InputManager::FormatKeyName(vk, name, sizeof(name));
        w = std::max(h, Measure(Txt::Caption, name) + pad * 2.0f);
        if (maxW > 0.0f && w > maxW) w = maxW;
    }
    if (!draw) return w;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(255, 255, 255, 14), U(kRadChip));
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), IM_COL32(255, 255, 255, 52), U(kRadChip), 0, std::max(1.0f, Uf(1.0f)));
    const ImVec2 c(p.x + w * 0.5f, p.y + h * 0.5f);
    if (IsArrowVk(vk)) {
        const Dir d = (vk == VK_UP) ? Dir::Up : (vk == VK_DOWN) ? Dir::Down : (vk == VK_LEFT) ? Dir::Left : Dir::Right;
        IconTriangle(dl, c, h * 0.42f, d, textCol);
    } else {
        const float tw = std::min(Measure(Txt::Caption, name), w - pad * 2.0f);
        TextFit(dl, Txt::Caption, ImVec2(p.x + (w - tw) * 0.5f, p.y + (h - LineH(Txt::Caption)) * 0.5f), w - pad * 2.0f,
                textCol, name);
    }
    return w;
}

float PadGlyph(ImDrawList* dl, ImVec2 p, float h, uint16_t mask, bool draw = true) {
    if (mask == 0) return 0.0f;
    const char* face = nullptr;
    ImU32 faceCol = C::Text;
    switch (mask) {
        case PadButtons::A: face = "A"; faceCol = IM_COL32(110, 205, 100, 255); break;
        case PadButtons::B: face = "B"; faceCol = IM_COL32(236, 96, 88, 255); break;
        case PadButtons::X: face = "X"; faceCol = IM_COL32(90, 156, 255, 255); break;
        case PadButtons::Y: face = "Y"; faceCol = IM_COL32(244, 200, 64, 255); break;
        default: break;
    }
    if (face) {
        if (!draw) return h;
        const ImVec2 c(p.x + h * 0.5f, p.y + h * 0.5f);
        dl->AddCircleFilled(c, h * 0.5f, IM_COL32(42, 48, 60, 255), 24);
        Text(dl, Txt::Caption, ImVec2(c.x - Measure(Txt::Caption, face) * 0.5f, c.y - LineH(Txt::Caption) * 0.5f), faceCol, face);
        return h;
    }
    char name[16];
    InputManager::FormatPadName(mask, name, sizeof(name));
    const float w = std::max(h * 1.3f, Measure(Txt::Caption, name) + U(10));
    if (!draw) return w;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(42, 48, 60, 255), h * 0.5f);
    Text(dl, Txt::Caption, ImVec2(p.x + (w - Measure(Txt::Caption, name)) * 0.5f, p.y + (h - LineH(Txt::Caption)) * 0.5f), C::Text2, name);
    return w;
}

// Shortest keyboard key of an action (arrows preferred for directions)
uint8_t PrimaryKey(const InputBindings& b, BindAction a) {
    const auto& k = b.keys[static_cast<size_t>(a)];
    if (k[0] == 0) return k[1];
    if (k[1] == 0) return k[0];
    if (IsArrowVk(k[0])) return k[0];
    if (IsArrowVk(k[1])) return k[1];
    char n0[32], n1[32];
    InputManager::FormatKeyName(k[0], n0, sizeof(n0));
    InputManager::FormatKeyName(k[1], n1, sizeof(n1));
    return (std::strlen(n1) < std::strlen(n0)) ? k[1] : k[0];
}

// Glyph for one action on the preferred device. Returns width (0 when unbound on that device).
float ActionGlyph(ImDrawList* dl, ImVec2 p, float h, const InputBindings& b, BindAction a, bool pad, bool draw = true,
                  ImU32 col = C::Text2) {
    if (pad) {
        // Only open/collapse has a gamepad shortcut; every other panel action is keyboard / mouse
        const uint16_t m = b.pad[static_cast<size_t>(a)];
        if (m != 0 && InputBindings::IsPadRemappable(a)) return PadGlyph(dl, p, h, m, draw);
    }
    return Keycap(dl, p, h, PrimaryKey(b, a), col, draw);
}

// Pair glyph for direction axes: up+down or left+right
float AxisGlyph(ImDrawList* dl, ImVec2 p, float h, const InputBindings& b, bool vertical, bool pad, bool draw = true) {
    (void)pad; // directions are keyboard only
    const BindAction a0 = vertical ? BindAction::Up : BindAction::Left;
    const BindAction a1 = vertical ? BindAction::Down : BindAction::Right;
    const float w0 = Keycap(dl, p, h, PrimaryKey(b, a0), C::Text2, draw);
    const float gap = U(3);
    const float w1 = Keycap(dl, ImVec2(p.x + w0 + gap, p.y), h, PrimaryKey(b, a1), C::Text2, draw);
    return w0 + gap + w1;
}

// ============================================================================
// Hint bar (footer)
// ============================================================================
struct Hint {
    enum Kind { Action, AxisV, AxisH, RawKey } kind;
    BindAction action;
    uint8_t vk;
    const char* label; // PT-BR, translated on draw
};

void HintBar(ImDrawList* dl, float contentW, const Hint* hints, int count) {
    const InputBindings& b = g_controller.Config().bindings;
    const bool pad = InputManager::PreferPadGlyphs();
    const float gh = U(20);
    const float itemGap = U(16);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(ImVec2(p0.x, std::floor(p0.y)), ImVec2(p0.x + contentW, std::floor(p0.y) + 1.0f), C::Hair);
    float x = p0.x, y = p0.y + U(10);
    float lineH = gh;
    for (int i = 0; i < count; ++i) {
        const Hint& h = hints[i];
        auto glyph = [&](ImVec2 at, bool draw) -> float {
            switch (h.kind) {
                case Hint::AxisV:  return AxisGlyph(dl, at, gh, b, true, pad, draw);
                case Hint::AxisH:  return AxisGlyph(dl, at, gh, b, false, pad, draw);
                case Hint::RawKey: return Keycap(dl, at, gh, h.vk, C::Text2, draw);
                default:           return ActionGlyph(dl, at, gh, b, h.action, pad, draw);
            }
        };
        const char* label = Tr(h.label);
        const float gw = glyph(ImVec2(0, 0), false);
        if (gw <= 0.0f) continue;
        const float itemW = gw + U(6) + Measure(Txt::Small, label);
        if (x > p0.x && x + itemW > p0.x + contentW) {
            x = p0.x;
            y += gh + U(6);
        }
        glyph(ImVec2(x, y), true);
        Text(dl, Txt::Small, ImVec2(x + gw + U(6), y + (gh - LineH(Txt::Small)) * 0.5f), C::Text2, label);
        x += itemW + itemGap;
    }
    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(ImVec2(contentW, (y - p0.y) + lineH + U(2)));
}

// ============================================================================
// Generic controls
// ============================================================================
bool HitArea(const char* id, ImVec2 p, ImVec2 sz, bool* hovered = nullptr) {
    ImGui::SetCursorScreenPos(p);
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, sz.x), std::max(1.0f, sz.y)));
    if (hovered) *hovered = ImGui::IsItemHovered();
    return clicked;
}

void Tooltip(const char* text) {
    ImGui::BeginTooltip();
    if (F.sm) ImGui::PushFont(F.sm);
    ImGui::TextUnformatted(text);
    if (F.sm) ImGui::PopFont();
    ImGui::EndTooltip();
}

// Square icon button (ghost). Returns clicked.
bool IconButton(ImDrawList* dl, const char* id, ImVec2 p, float sz, void (*icon)(ImDrawList*, ImVec2, float, ImU32),
                const char* tooltip, bool filled = true) {
    bool hov = false;
    const bool clicked = HitArea(id, p, ImVec2(sz, sz), &hov);
    if (filled || hov) {
        dl->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz), hov ? IM_COL32(255, 255, 255, 26) : IM_COL32(255, 255, 255, 10), U(kRadCtl));
    }
    icon(dl, ImVec2(p.x + sz * 0.5f, p.y + sz * 0.5f), sz * 0.46f, hov ? C::Text : C::Text2);
    if (hov && tooltip) Tooltip(tooltip);
    return clicked;
}

// Selectable list row background. Returns clicked; reports hover.
// button=false: hover only (for rows whose own controls, e.g. "< value >", must receive the clicks)
bool RowBase(ImDrawList* dl, const char* id, ImVec2 p, ImVec2 sz, bool selected, bool* hovered, bool surface = true,
             bool button = true) {
    bool hov = false;
    bool clicked = false;
    if (button) clicked = HitArea(id, p, sz, &hov);
    else hov = ImGui::IsMouseHoveringRect(p, ImVec2(p.x + sz.x, p.y + sz.y), true);
    if (hovered) *hovered = hov;
    const ImVec2 e(p.x + sz.x, p.y + sz.y);
    if (selected) {
        dl->AddRectFilled(p, e, C::Selected, U(kRadCtl));
        EdgeAccent(dl, p, e, C::Accent, U(kRadCtl), U(3));
    } else if (hov) {
        dl->AddRectFilled(p, e, C::SurfaceHi, U(kRadCtl));
    } else if (surface) {
        dl->AddRectFilled(p, e, C::Surface, U(kRadCtl));
    }
    return clicked;
}

void Toggle(ImDrawList* dl, ImVec2 rightCenter, bool on) {
    const float w = U(36), h = U(20);
    const ImVec2 a(rightCenter.x - w, rightCenter.y - h * 0.5f);
    const ImVec2 b(rightCenter.x, rightCenter.y + h * 0.5f);
    dl->AddRectFilled(a, b, on ? C::Accent : IM_COL32(255, 255, 255, 42), h * 0.5f);
    const float r = h * 0.5f - U(3);
    const float cx = on ? (b.x - h * 0.5f) : (a.x + h * 0.5f);
    dl->AddCircleFilled(ImVec2(cx, rightCenter.y), r, IM_COL32(255, 255, 255, 255), 20);
}

// "<  value  >" selector at the right side of a row. Returns -1 / +1 when a chevron or the value is clicked.
int ValueSelector(ImDrawList* dl, const char* id, ImVec2 rowP, ImVec2 rowSz, float valueW, const char* value,
                  ImU32 valueCol, bool active, RgbColor* dot = nullptr) {
    int delta = 0;
    const float btn = U(26);
    const float right = rowP.x + rowSz.x - U(8);
    const float cy = rowP.y + rowSz.y * 0.5f;
    const ImVec2 nextP(right - btn, cy - btn * 0.5f);
    const ImVec2 valP(nextP.x - valueW, rowP.y);
    const ImVec2 prevP(valP.x - btn, cy - btn * 0.5f);
    ImGui::PushID(id);
    bool hp = false, hn = false, hv = false;
    if (HitArea("##p", prevP, ImVec2(btn, btn), &hp)) delta = -1;
    if (HitArea("##v", valP, ImVec2(valueW, rowSz.y), &hv)) delta = +1;
    if (HitArea("##n", nextP, ImVec2(btn, btn), &hn)) delta = +1;
    ImGui::PopID();
    const ImU32 chevCol = active ? C::Text : C::Text3;
    if (hp) dl->AddRectFilled(prevP, ImVec2(prevP.x + btn, prevP.y + btn), C::SurfaceHi, U(kRadChip));
    if (hn) dl->AddRectFilled(nextP, ImVec2(nextP.x + btn, nextP.y + btn), C::SurfaceHi, U(kRadChip));
    IconChevron(dl, ImVec2(prevP.x + btn * 0.5f, cy), U(10), Dir::Left, hp ? C::Text : chevCol);
    IconChevron(dl, ImVec2(nextP.x + btn * 0.5f, cy), U(10), Dir::Right, hn ? C::Text : chevCol);

    const float dotW = dot ? U(16) : 0.0f;
    const float maxTextW = valueW - U(8) - dotW;
    const float tw = std::min(Measure(Txt::Strong, value), maxTextW);
    float x = valP.x + (valueW - tw - dotW) * 0.5f;
    if (dot) {
        dl->AddCircleFilled(ImVec2(x + U(5), cy), U(5), RgbToImU32(*dot), 16);
        x += dotW;
    }
    TextFit(dl, Txt::Strong, ImVec2(x, cy - LineH(Txt::Strong) * 0.5f), maxTextW, (hv && !active) ? C::Text : valueCol, value);
    return delta;
}

void SectionCaption(ImDrawList* dl, const char* ptText, float topGap) {
    if (topGap > 0.0f) ImGui::Dummy(ImVec2(0, topGap));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    Tracked(dl, ImVec2(p.x + U(2), p.y), C::Text3, Tr(ptText));
    ImGui::Dummy(ImVec2(0, LineH(Txt::Caption) + U(8)));
}

ImU32 MarkerColor(const ReplayMarker& m) {
    // A contact that put a player down with an identified opponent and no whistle: near foul (yellow)
    if (!m.isOfficialEvent && m.nearFoul) return C::Yellow;
    switch (m.kind) {
        case IncidentKind::Penalty:
        case IncidentKind::RedCard:    return C::Red;
        case IncidentKind::Foul:
        case IncidentKind::YellowCard:
        case IncidentKind::Advantage:  return C::Yellow;
        case IncidentKind::Dive:       return C::Orange;
        default:                       return C::Neutral;
    }
}

// Pitch token: shadow, dark hairline (separates any kit from the grass), team fill, contrast ring and a bold
// number with a 1 px halo in the opposite tone. Readable for white, black and green kits alike.
float ColorDistance(RgbColor a, RgbColor b) {
    const float dr = static_cast<float>(a.r) - b.r, dg = static_cast<float>(a.g) - b.g, db = static_cast<float>(a.b) - b.b;
    return std::sqrt(dr * dr + dg * dg + db * db);
}

void PitchToken(ImDrawList* dl, ImFont* numFont, ImVec2 c, float r, const TeamDisplayInfo& t, uint8_t number, float alpha) {
    const RgbColor fill = t.primaryColor;
    RgbColor ring = t.secondaryColor;
    if (ColorDistance(ring, fill) < 90.0f) {
        ring = (ColorHelpers::RelativeLuma(fill) > 140.0f) ? RgbColor{ 18, 24, 32 } : RgbColor{ 245, 248, 252 };
    }
    const float edge = std::max(1.0f, r * 0.16f);
    dl->AddCircleFilled(ImVec2(c.x, c.y + std::max(1.0f, r * 0.14f)), r + edge + 1.0f, WithAlpha(IM_COL32(0, 0, 0, 120), alpha), 0);
    dl->AddCircleFilled(c, r + edge + 0.8f, WithAlpha(IM_COL32(6, 10, 14, 235), alpha), 0);
    dl->AddCircleFilled(c, r + edge, WithAlpha(RgbToImU32(ring), alpha), 0);
    dl->AddCircleFilled(c, r, WithAlpha(RgbToImU32(fill), alpha), 0);
    if (number == 0 || !numFont || r < 4.5f) return;
    char nb[8];
    std::snprintf(nb, sizeof(nb), "%u", static_cast<unsigned>(number));
    const float size = std::max(7.0f, r * (number >= 10 ? 1.18f : 1.34f));
    const ImVec2 ns = numFont->CalcTextSizeA(size, FLT_MAX, 0.0f, nb);
    const ImVec2 tp = Snap(ImVec2(c.x - ns.x * 0.5f, c.y - ns.y * 0.52f));
    const ImU32 txt = ContrastTextForRgb(fill);
    const ImU32 halo = WithAlpha((txt == IM_COL32(14, 20, 30, 255)) ? IM_COL32(255, 255, 255, 110) : IM_COL32(0, 0, 0, 150), alpha);
    dl->AddText(numFont, size, ImVec2(tp.x + 1.0f, tp.y + 1.0f), halo, nb);
    dl->AddText(numFont, size, tp, WithAlpha(txt, alpha), nb);
}

// Paints the area between a rectangle's square corners and its rounded corners with the colour behind the card,
// so content drawn edge to edge (the pitch) follows the card radius
void MaskOuterCorners(ImDrawList* dl, ImVec2 p, ImVec2 e, float radius, ImU32 bg) {
    if (radius < 1.0f) return;
    constexpr int kSeg = 8;
    const ImVec2 corners[4] = { p, ImVec2(e.x, p.y), e, ImVec2(p.x, e.y) };
    const ImVec2 centres[4] = { ImVec2(p.x + radius, p.y + radius), ImVec2(e.x - radius, p.y + radius),
                                ImVec2(e.x - radius, e.y - radius), ImVec2(p.x + radius, e.y - radius) };
    const float start[4] = { 3.14159265f, 1.5f * 3.14159265f, 0.0f, 0.5f * 3.14159265f };
    for (int c = 0; c < 4; ++c) {
        ImVec2 prev(centres[c].x + std::cos(start[c]) * radius, centres[c].y + std::sin(start[c]) * radius);
        for (int i = 1; i <= kSeg; ++i) {
            const float a = start[c] + 0.5f * 3.14159265f * static_cast<float>(i) / kSeg;
            const ImVec2 cur(centres[c].x + std::cos(a) * radius, centres[c].y + std::sin(a) * radius);
            dl->AddTriangleFilled(corners[c], prev, cur, bg);
            prev = cur;
        }
    }
}

void PlayerChip(ImDrawList* dl, ImVec2 c, float r, const TeamDisplayInfo& t) {
    dl->AddCircleFilled(c, r, RgbToImU32(t.primaryColor), 20);
    dl->AddCircle(c, r, RgbToImU32(t.secondaryColor), 20, std::max(1.0f, Uf(1.4f)));
}

void PanelShadow(ImVec2 a, ImVec2 b, float radius) {
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    // Each layer is a ring from just inside the panel edge out to its spread: the panel (opaque) hides the inner
    // half, so the result matches the old stacked fills while the GPU blends only a band around the panel
    for (int i = 1; i <= 6; ++i) {
        const float g = Uf(1.6f) * static_cast<float>(i);
        // Outer bounds = the old filled layer; thick enough that the inner edge always lies under the panel,
        // also at the bottom where the layer is offset downwards
        const float t = g + Uf(4.0f) + 1.0f;
        const float h = t * 0.5f;
        bg->AddRect(ImVec2(a.x - g + h, a.y - g + Uf(2.0f) + h), ImVec2(b.x + g - h, b.y + g + Uf(3.0f) - h),
                    IM_COL32(0, 0, 0, 22 - i * 3), std::max(0.0f, radius + g - h), 0, t);
    }
}


// ============================================================================
// Banners, collapsed pill, debug telemetry
// ============================================================================
void RenderBanner(float vw, float vh, const char* storedText, uint64_t startMs, uint64_t untilMs, BannerStyle style, bool isAlert) {
    const uint64_t now = GetTickCount64();
    if (!storedText || !storedText[0] || now >= untilMs) return;
    float alpha = 1.0f;
    if (now < startMs + 220) alpha = static_cast<float>(now - startMs) / 220.0f;
    else if (untilMs - now < 350) alpha = static_cast<float>(untilMs - now) / 350.0f;
    alpha = std::clamp(alpha, 0.0f, 1.0f);

    const char* text = Tr(storedText);
    const char* sep = std::strstr(text, "  \xC2\xB7  ");
    std::string title = sep ? std::string(text, sep) : std::string(text);
    const char* detail = sep ? sep + 6 : nullptr;

    const ImU32 accent = (style == BannerStyle::RedCard) ? C::Red
                       : (style == BannerStyle::Amber)   ? C::Yellow
                       : (style == BannerStyle::Info)    ? C::Accent
                       : C::Green;

    const float h = U(44);
    float badgeW = 0.0f;
    const float titleW = TrackedW(title.c_str());
    const float detailW = detail ? Measure(Txt::Small, detail) : 0.0f;
    const float badgeMeasure = Measure(Txt::Strong, "VAR") + U(14);
    float w = U(14) + badgeMeasure + U(14) + titleW + (detail ? U(14) + detailW : 0.0f) + U(18);
    float detailMaxW = detailW;
    if (w > vw - U(32)) {
        detailMaxW = std::max(0.0f, detailW - (w - (vw - U(32))));
        w = vw - U(32);
    }
    const float slide = (1.0f - alpha) * U(-10);
    const ImVec2 p(CenterOutsidePanel(vw, w), (isAlert ? U(72) : U(24)) + slide);
    const ImVec2 e(p.x + w, p.y + h);

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    for (int i = 1; i <= 4; ++i) {
        const float g = Uf(1.6f) * static_cast<float>(i);
        dl->AddRectFilled(ImVec2(p.x - g, p.y - g + Uf(2)), ImVec2(e.x + g, e.y + g + Uf(3)), WithAlpha(IM_COL32(0, 0, 0, 20 - i * 4), alpha), U(kRadCard) + g);
    }
    dl->AddRectFilled(p, e, WithAlpha(C::PanelSolid, alpha * 0.96f), U(kRadCard));
    dl->AddRect(p, e, WithAlpha(C::Stroke, alpha), U(kRadCard), 0, 1.0f);
    EdgeAccent(dl, p, e, WithAlpha(accent, alpha), U(kRadCard), U(3));

    float x = p.x + U(14);
    const float bh = U(24);
    dl->AddRectFilled(ImVec2(x, p.y + (h - bh) * 0.5f), ImVec2(x + badgeMeasure, p.y + (h + bh) * 0.5f), WithAlpha(C::Accent, alpha), U(5));
    Text(dl, Txt::Strong, ImVec2(x + U(7), p.y + (h - LineH(Txt::Strong)) * 0.5f - Uf(0.5f)), WithAlpha(IM_COL32(255, 255, 255, 255), alpha), "VAR");
    (void)badgeW;
    x += badgeMeasure + U(14);
    Tracked(dl, ImVec2(x, p.y + (h - LineH(Txt::Caption)) * 0.5f), WithAlpha(C::Text, alpha), title.c_str());
    x += titleW;
    if (detail) {
        x += U(14);
        TextFit(dl, Txt::Small, ImVec2(x, p.y + (h - LineH(Txt::Small)) * 0.5f), detailMaxW, WithAlpha(C::Text2, alpha), detail);
    }
}

void RenderDebugTelemetry(float vw, float vh, const LiveTelemetry& tel, const VarVerdict& pending) {
    ImGui::SetNextWindowPos(ImVec2(U(20), U(20)), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(U(460), 0.0f), ImGuiCond_Always);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##VarDebugV5", nullptr, kFlags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float w = ImGui::GetContentRegionAvail().x;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        Tracked(dl, p, C::Accent, Tr("TELEMETRIA"));
        char key[32], closes[64];
        InputManager::FormatKeyName(g_controller.Config().bindings.keys[static_cast<size_t>(BindAction::ToggleDebug)][0], key, sizeof(key));
        std::snprintf(closes, sizeof(closes), Tr("%s fecha"), key);
        Text(dl, Txt::Small, ImVec2(p.x + w - Measure(Txt::Small, closes), p.y - Uf(1)), C::Text3, closes);
        ImGui::Dummy(ImVec2(w, LineH(Txt::Caption) + U(8)));

        char line[200];
        auto row = [&](const char* s) {
            const ImVec2 rp = ImGui::GetCursorScreenPos();
            TextFit(dl, Txt::Small, rp, w, C::Text2, s);
            ImGui::Dummy(ImVec2(w, LineH(Txt::Small) + U(4)));
        };
        std::snprintf(line, sizeof(line), Tr("Resolução %.0fx%.0f  ·  hooks %s  ·  seek %s"), vw, vh,
                      tel.hooksInstalled ? "OK" : Tr("aguardando"), tel.replaySeekReady ? Tr("nativo") : Tr("passivo"));
        row(line);
        std::snprintf(line, sizeof(line), Tr("Times %s x %s  ·  status %d  ·  FSM 0x%X"),
                      g_controller.TeamShortName(0), g_controller.TeamShortName(1), tel.matchStatus, tel.fsmState);
        row(line);
        std::snprintf(line, sizeof(line), "Replay mode=%u manual=%u play=%u norm=%.2f focus=%d",
                      tel.replayMode, tel.replayManualFlag, tel.replayIsPlaying, tel.replayTimelineNorm, tel.replayFocusSlot);
        row(line);
        std::snprintf(line, sizeof(line), Tr("Marcadores %zu  ·  bola (%.1f, %.1f)"),
                      g_controller.ReplayMarkers().size(), tel.liveBallPos.x, tel.liveBallPos.z);
        row(line);
        std::snprintf(line, sizeof(line), Tr("Decisão: %s"), pending.armed ? pending.summary : Tr("nenhuma"));
        row(line);
    }
    ImGui::End();
}

void RenderCollapsedPill(float vw, float vh, const VarVerdict& pending, const VerdictPool& pool, const HudConfig& cfg) {
    (void)vh;
    const float margin = U(20);
    const size_t poolCount = pool.Count();
    // Rebuilt only when the pool or the language changes (no per-frame string building while collapsed)
    static std::string s_pillCache;
    static uint64_t s_pillKey = ~0ULL;
    const uint64_t pillKey = (static_cast<uint64_t>(pool.sequence) << 24) ^ (static_cast<uint64_t>(poolCount) << 16) ^
                             (static_cast<uint64_t>(pool.CardCount()) << 8) ^
                             (static_cast<uint64_t>(i18n::GetLanguage() == Language::English ? 1 : 0) << 7) ^
                             (static_cast<uint64_t>(pending.sequence) << 40) ^ (pool.hasPlayDecision ? 0x40ULL : 0ULL) ^
                             (g_frameNamesRevision * 0x9E3779B97F4A7C15ULL);
    if (pillKey != s_pillKey) {
        s_pillKey = pillKey;
        s_pillCache = poolCount > 0 ? g_controller.FormatPoolSummary(pool) : std::string(Tr(pending.summary));
    }
    const char* pillSummary = s_pillCache.c_str();
    const bool armed = pending.armed || poolCount > 0;
    const bool pad = InputManager::PreferPadGlyphs();
    const float h = U(44);
    const float gh = U(20);

    char poolHdr[64];
    if (poolCount > 1) {
        std::snprintf(poolHdr, sizeof(poolHdr), Tr("POOL PRONTO (%zu)"), poolCount);
    } else {
        std::snprintf(poolHdr, sizeof(poolHdr), "%s", Tr("DECISÃO PRONTA"));
    }

    const float badgeW = Measure(Txt::Strong, "VAR") + U(14);
    float contentW = 0.0f;
    if (armed) {
        contentW = std::max(TrackedW(poolHdr), std::min(Measure(Txt::Strong, pillSummary), U(340)));
    } else {
        contentW = Measure(Txt::Small, Tr("Abrir painel")) + U(8) +
                   ActionGlyph(nullptr, ImVec2(0, 0), gh, cfg.bindings, BindAction::ToggleFocus, pad, false);
    }
    const float w = U(12) + badgeW + U(12) + contentW + U(14);
    const float px = cfg.dockLeft ? margin : (vw - margin - w);
    const ImVec2 p(px, margin);
    const ImVec2 e(p.x + w, p.y + (armed ? U(52) : h));

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    for (int i = 1; i <= 4; ++i) {
        const float g = Uf(1.6f) * static_cast<float>(i);
        dl->AddRectFilled(ImVec2(p.x - g, p.y - g + Uf(2)), ImVec2(e.x + g, e.y + g + Uf(3)), IM_COL32(0, 0, 0, 20 - i * 4), U(kRadCard) + g);
    }
    dl->AddRectFilled(p, e, C::Panel, U(kRadCard));
    dl->AddRect(p, e, armed ? WithAlpha(C::Green, 0.55f) : C::Stroke, U(kRadCard), 0, 1.0f);
    const float bh = U(24);
    VarBadge(dl, ImVec2(p.x + U(12), p.y + ((e.y - p.y) - bh) * 0.5f), bh);
    const float x = p.x + U(12) + badgeW + U(12);
    if (armed) {
        Tracked(dl, ImVec2(x, p.y + U(9)), C::Green, poolHdr);
        TextFit(dl, Txt::Strong, ImVec2(x, p.y + U(9) + LineH(Txt::Caption) + U(3)), U(340), C::Text, pillSummary);
    } else {
        const char* lbl = Tr("Abrir painel");
        Text(dl, Txt::Small, ImVec2(x, p.y + (h - LineH(Txt::Small)) * 0.5f), C::Text2, lbl);
        ActionGlyph(dl, ImVec2(x + Measure(Txt::Small, lbl) + U(8), p.y + (h - gh) * 0.5f), gh, cfg.bindings, BindAction::ToggleFocus, pad);
    }
}

// ============================================================================
// Timeline
// ============================================================================
void RenderTimeline(ImDrawList* dl, float contentW, VarState& state) {
    const auto& tel = g_controller.Telemetry();
    const auto& markers = g_controller.ReplayMarkers();
    const int selIdx = g_controller.SelectedMarkerIndex();
    const int total = static_cast<int>(markers.size());
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float h = U(32);
    const float cy = p0.y + h * 0.5f;

    // Play / pause
    const float pb = U(32);
    bool playHov = false;
    if (HitArea("##tl_play", p0, ImVec2(pb, pb), &playHov)) g_controller.ToggleReplayPlayPause(state);
    dl->AddCircleFilled(ImVec2(p0.x + pb * 0.5f, cy), pb * 0.5f, playHov ? C::SurfaceHi : C::Surface, 28);
    if (tel.replayIsPlaying) IconPause(dl, ImVec2(p0.x + pb * 0.5f, cy), U(11), C::Text);
    else IconPlay(dl, ImVec2(p0.x + pb * 0.5f + Uf(1), cy), U(12), C::Text);
    if (playHov) Tooltip(tel.replayIsPlaying ? Tr("Pausar") : Tr("Reproduzir"));

    // Prev / counter / next (right)
    const float sb = U(26);
    char counter[24];
    if (total > 0 && selIdx >= 0) std::snprintf(counter, sizeof(counter), "%d/%d", selIdx + 1, total);
    else std::snprintf(counter, sizeof(counter), "%d", total);
    const float cw = std::max(Measure(Txt::Small, "00/00"), Measure(Txt::Small, counter));
    const float nextX = p0.x + contentW - sb;
    const float cntX = nextX - U(2) - cw;
    const float prevX = cntX - U(2) - sb;
    bool hp = false, hn = false;
    if (HitArea("##tl_prev", ImVec2(prevX, cy - sb * 0.5f), ImVec2(sb, sb), &hp) && total > 0) g_controller.StepReplayMarker(-1, state);
    if (HitArea("##tl_next", ImVec2(nextX, cy - sb * 0.5f), ImVec2(sb, sb), &hn) && total > 0) g_controller.StepReplayMarker(+1, state);
    if (hp && total > 0) dl->AddRectFilled(ImVec2(prevX, cy - sb * 0.5f), ImVec2(prevX + sb, cy + sb * 0.5f), C::SurfaceHi, U(kRadChip));
    if (hn && total > 0) dl->AddRectFilled(ImVec2(nextX, cy - sb * 0.5f), ImVec2(nextX + sb, cy + sb * 0.5f), C::SurfaceHi, U(kRadChip));
    const ImU32 stepCol = (total > 0) ? C::Text : C::Text3;
    IconChevron(dl, ImVec2(prevX + sb * 0.5f, cy), U(10), Dir::Left, stepCol);
    IconChevron(dl, ImVec2(nextX + sb * 0.5f, cy), U(10), Dir::Right, stepCol);
    Text(dl, Txt::Small, ImVec2(cntX + (cw - Measure(Txt::Small, counter)) * 0.5f, cy - LineH(Txt::Small) * 0.5f),
         (total > 0) ? C::Text2 : C::Text3, counter);
    if (hp) Tooltip(Tr("Lance anterior"));
    if (hn) Tooltip(Tr("Próximo lance"));

    // Clocks + track
    int sm = 0, ss = 0, em = 0, es = 0;
    PitchGeometry::FormatMatchClock(tel.replayWindowStartPhase, tel.replayWindowStartTime, sm, ss);
    PitchGeometry::FormatMatchClock(tel.replayWindowEndPhase, tel.replayWindowEndTime, em, es);
    char c0[16], c1[16];
    std::snprintf(c0, sizeof(c0), "%02d:%02d", sm, ss);
    std::snprintf(c1, sizeof(c1), "%02d:%02d", em, es);
    const float clkW = Measure(Txt::Caption, "00:00");
    const float x0 = p0.x + pb + U(10);
    Text(dl, Txt::Caption, ImVec2(x0, cy - LineH(Txt::Caption) * 0.5f), C::Text3, c0);
    const float trackX0 = x0 + clkW + U(10);
    const float trackX1 = prevX - U(10) - clkW - U(10);
    Text(dl, Txt::Caption, ImVec2(trackX1 + U(10), cy - LineH(Txt::Caption) * 0.5f), C::Text3, c1);
    const float trackW = std::max(U(40), trackX1 - trackX0);
    const float th = U(4);

    float norm = std::clamp(tel.replayTimelineNorm, 0.0f, 1.0f);
    if (norm <= 0.0001f && tel.replayWindowEndTime > tel.replayWindowStartTime + 1.0f) {
        norm = std::clamp((tel.replayCursorTime - tel.replayWindowStartTime) / (tel.replayWindowEndTime - tel.replayWindowStartTime), 0.0f, 1.0f);
    }
    const float playX = trackX0 + norm * trackW;
    dl->AddRectFilled(ImVec2(trackX0, cy - th * 0.5f), ImVec2(trackX0 + trackW, cy + th * 0.5f), IM_COL32(255, 255, 255, 30), th * 0.5f);
    dl->AddRectFilled(ImVec2(trackX0, cy - th * 0.5f), ImVec2(playX, cy + th * 0.5f), C::Accent, th * 0.5f);

    ImGui::SetCursorScreenPos(ImVec2(trackX0 - U(6), cy - U(12)));
    ImGui::InvisibleButton("##tl_scrub", ImVec2(trackW + U(12), U(24)));
    const bool trackHovered = ImGui::IsItemHovered();
    const bool trackClicked = ImGui::IsItemClicked(0);
    const bool trackDragging = ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 2.0f);

    const ImVec2 mp = ImGui::GetIO().MousePos;
    int hovered = -1;
    float best = U(10);
    for (int i = 0; i < total; ++i) {
        const float mx = trackX0 + std::clamp(markers[i].timelineNorm, 0.0f, 1.0f) * trackW;
        const float d = std::fabs(mp.x - mx);
        if (trackHovered && d < best) { best = d; hovered = i; }
    }
    static float s_lastDragNorm = -1.0f;
    if (trackClicked) {
        if (hovered >= 0) {
            g_controller.SelectReplayMarker(hovered, state, true);
            s_lastDragNorm = -1.0f;
        } else {
            const float n = std::clamp((mp.x - trackX0) / trackW, 0.0f, 1.0f);
            s_lastDragNorm = n;
            g_controller.ScrubReplayToNorm(n, state);
        }
    } else if (trackDragging) {
        const float n = std::clamp((mp.x - trackX0) / trackW, 0.0f, 1.0f);
        if (std::fabs(n - s_lastDragNorm) >= 0.004f) {
            s_lastDragNorm = n;
            g_controller.ScrubReplayToNorm(n, state);
        }
    }

    // The suspect pulses (two sonar rings) until the user opens it once in this replay window. Afterwards it
    // keeps a quiet gold ring, so it stays findable without competing with the selection.
    int suspectIdx = -1;
    for (int i = 0; i < total; ++i) {
        if (markers[i].topSuspect) { suspectIdx = i; break; }
    }
    const bool suspectSeen = suspectIdx >= 0 && g_controller.WasPlayOpened(markers[suspectIdx]);

    for (int i = 0; i < total; ++i) {
        const auto& m = markers[i];
        const float mx = trackX0 + std::clamp(m.timelineNorm, 0.0f, 1.0f) * trackW;
        const bool emph = (i == selIdx) || (i == hovered);
        const float r = emph ? Uf(5.5f) : (m.isOfficialEvent ? Uf(4.5f) : Uf(3.6f));
        if (i == suspectIdx) {
            if (!suspectSeen) {
                const float t = static_cast<float>(ImGui::GetTime());
                for (int k = 0; k < 2; ++k) {
                    const float ph = std::fmod(t / 1.6f + 0.5f * static_cast<float>(k), 1.0f);
                    const float rr = std::min(r + Uf(2.0f) + ph * Uf(11.0f), h * 0.5f - 1.0f);
                    const int a = static_cast<int>(200.0f * (1.0f - ph) * (1.0f - ph));
                    dl->AddCircle(ImVec2(mx, cy), rr, (C::Suspect & 0x00FFFFFFu) | (static_cast<ImU32>(a) << 24), 24,
                                  std::max(1.2f, Uf(1.6f)));
                }
            }
            dl->AddCircle(ImVec2(mx, cy), r + Uf(2.4f), C::Suspect, 20, std::max(1.2f, Uf(1.5f)));
        }
        dl->AddCircleFilled(ImVec2(mx, cy), r + Uf(1.6f), C::PanelSolid, 16);
        dl->AddCircleFilled(ImVec2(mx, cy), r, MarkerColor(m), 16);
        if (emph) dl->AddCircle(ImVec2(mx, cy), r + Uf(3.0f), IM_COL32(255, 255, 255, 220), 20, std::max(1.2f, Uf(1.4f)));
    }
    dl->AddCircleFilled(ImVec2(playX, cy), Uf(6.5f), IM_COL32(0, 0, 0, 90), 20);
    dl->AddCircleFilled(ImVec2(playX, cy), Uf(5.5f), IM_COL32(255, 255, 255, 255), 20);

    if (hovered >= 0) {
        const auto& m = markers[hovered];
        int mm = 0, ms = 0;
        PitchGeometry::FormatMatchClock(m.matchPhase, m.time60Hz, mm, ms);
        char off[80];
        g_controller.FormatPlayerBroadcastBuf(m.offenderSlot, off, sizeof(off));
        ImGui::BeginTooltip();
        if (m.topSuspect) {
            if (F.sm) ImGui::PushFont(F.sm);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(C::Suspect), "%s", Tr("PROVÁVEL FALTA NÃO MARCADA"));
            if (F.sm) ImGui::PopFont();
        }
        if (F.strong) ImGui::PushFont(F.strong);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(MarkerColor(m)), "%02d:%02d  \xC2\xB7  %s", mm, ms, Tr(m.label));
        if (F.strong) ImGui::PopFont();
        if (F.sm) ImGui::PushFont(F.sm);
        if (IsValidSlot(m.victimSlot)) {
            char vic[80];
            g_controller.FormatPlayerBroadcastBuf(m.victimSlot, vic, sizeof(vic));
            ImGui::Text((m.rolesConfirmed && m.kind != IncidentKind::Dive) ? Tr("%s em %s") : Tr("%s x %s"), off, vic);
        } else {
            ImGui::TextUnformatted(off);
        }
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(C::Text3), "%s", Tr("Clique para ir ao lance"));
        if (F.sm) ImGui::PopFont();
        ImGui::EndTooltip();
    }

    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(ImVec2(contentW, h));
}

// ============================================================================
// Incident card
// ============================================================================
void CameraIconAdapter(ImDrawList* dl, ImVec2 c, float sz, ImU32 col) { IconCamera(dl, c, sz, col); }
void SwapIconAdapter(ImDrawList* dl, ImVec2 c, float sz, ImU32 col) { IconSwap(dl, c, sz, col); }

void RenderIncidentCard(ImDrawList* dl, float contentW, VarState& state) {
    const auto& players = g_controller.Players();
    const auto& teams = g_controller.Teams();
    const int32_t offSlot = g_controller.DetectedOffenderSlot();
    const int32_t vicSlot = g_controller.DetectedVictimSlot();
    const bool rolesKnown = g_controller.DetectedRolesConfirmed();
    const float boxDist = g_controller.ActiveSignedBoxDistance();
    const bool inBox = (boxDist < 0.0f);

    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float pad = U(12);
    const float row1H = LineH(Txt::Caption);
    const float nameH = LineH(Txt::Strong);
    const float h = pad + row1H + U(12) + LineH(Txt::Caption) + U(4) + nameH + pad;
    const ImVec2 e(p.x + contentW, p.y + h);
    dl->AddRectFilled(p, e, C::Surface, U(kRadCard));

    const IncidentKind kind = g_controller.DetectedIncidentKind();
    const ImU32 sev = (kind == IncidentKind::Penalty || kind == IncidentKind::RedCard) ? C::Red
                    : g_controller.HasActiveEvent() ? C::Yellow
                    : g_controller.HasDetectedContact() ? C::Orange
                    : C::Text3;
    const char* title = g_controller.DetectedIncidentLabel()[0] ? g_controller.DetectedIncidentLabel() : Tr("JOGO SEGUE");

    // Row 1: severity dot + title ........ zone
    const float y1 = p.y + pad;
    char zone[64], meters[24];
    if (inBox) {
        FormatMeters(meters, sizeof(meters), std::fabs(boxDist));
        std::snprintf(zone, sizeof(zone), "%s %s", meters, Tr("DENTRO DA ÁREA")); // the distance says what it measures
    } else if (boxDist < 30.0f) {
        FormatMeters(meters, sizeof(meters), boxDist);
        std::snprintf(zone, sizeof(zone), "%s %s", meters, Tr("FORA DA ÁREA"));
    } else {
        std::snprintf(zone, sizeof(zone), "%s", Tr("MEIO-CAMPO"));
    }
    // The suspect play says so in words (the gold pulse alone is not self-explanatory)
    const auto& zoneMarkers = g_controller.ReplayMarkers();
    const int zoneSel = g_controller.SelectedMarkerIndex();
    const bool zoneSuspect = g_controller.SelectedMarkerApplied() && zoneSel >= 0 &&
                             zoneSel < static_cast<int>(zoneMarkers.size()) && zoneMarkers[static_cast<size_t>(zoneSel)].topSuspect;
    if (zoneSuspect) std::snprintf(zone, sizeof(zone), "%s", Tr("PROVÁVEL FALTA NÃO MARCADA"));
    const float zoneW = TrackedW(zone);
    Tracked(dl, ImVec2(e.x - pad - zoneW, y1), zoneSuspect ? C::Suspect : inBox ? C::Red : C::Text3, zone);
    // The play under review is the suspect of this replay: gold dot with a ring, matching its timeline marker
    const auto& cardMarkers = g_controller.ReplayMarkers();
    const int cardSel = g_controller.SelectedMarkerIndex();
    const bool cardSuspect = g_controller.SelectedMarkerApplied() && cardSel >= 0 &&
                             cardSel < static_cast<int>(cardMarkers.size()) && cardMarkers[static_cast<size_t>(cardSel)].topSuspect;
    dl->AddCircleFilled(ImVec2(p.x + pad + U(4), y1 + row1H * 0.5f), U(4), cardSuspect ? C::Suspect : sev, 12);
    if (cardSuspect) dl->AddCircle(ImVec2(p.x + pad + U(4), y1 + row1H * 0.5f), U(6.5f), C::Suspect, 16, std::max(1.0f, Uf(1.2f)));
    const float titleMaxW = std::max(U(40), contentW - pad * 2 - zoneW - U(26));
    // Title uses tracked caption in primary colour (clipped)
    TrackedFit(dl, ImVec2(p.x + pad + U(14), y1), C::Text, title, titleMaxW);

    // Row 2: offender -> victim  [camera] [swap]
    const float btn = U(34);
    const float btnY = y1 + row1H + U(12) + (LineH(Txt::Caption) + U(4) + nameH - btn) * 0.5f;
    const float swapX = e.x - pad - btn;
    const float camX = swapX - U(6) - btn;
    char focusLabel[48];
    g_controller.FormatNextCameraFocusLabelBuf(focusLabel, sizeof(focusLabel));
    if (IconButton(dl, "##cam_focus", ImVec2(camX, btnY), btn, CameraIconAdapter, focusLabel)) g_controller.CycleCameraFocus(state);
    if (IconButton(dl, "##swap_roles", ImVec2(swapX, btnY), btn, SwapIconAdapter,
                   g_controller.DetectedRolesConfirmed() ? Tr("Trocar infrator e vítima") : Tr("Definir infrator e vítima"))) {
        g_controller.InvertDetectedRoles();
    }

    const float namesL = p.x + pad;
    const float namesR = camX - U(12);
    const float arrowW = U(20);
    const float colW = std::max(U(60), (namesR - namesL - arrowW) * 0.5f);
    const float y2 = y1 + row1H + U(12);
    auto person = [&](float x0, const char* role, ImU32 roleCol, int32_t slot) {
        Tracked(dl, ImVec2(x0, y2), roleCol, role);
        const float ny = y2 + LineH(Txt::Caption) + U(4);
        if (IsValidSlot(slot)) {
            const auto& t = teams[players[slot].team == 1 ? 1 : 0];
            PlayerChip(dl, ImVec2(x0 + U(6), ny + nameH * 0.5f), U(6), t);
            char name[80];
            g_controller.FormatPlayerBroadcastBuf(slot, name, sizeof(name));
            // "#8 ANDREAS PEREIRA" too long for the column: "#8 PEREIRA" instead of a cut name
            if (Measure(Txt::Strong, name) > colW - U(18)) {
                const char* lastSpace = std::strrchr(name, ' ');
                const char* firstSpace = std::strchr(name, ' ');
                if (lastSpace && firstSpace && lastSpace != firstSpace) {
                    char shortName[80];
                    std::snprintf(shortName, sizeof(shortName), "%.*s%s", static_cast<int>(firstSpace - name + 1), name, lastSpace + 1);
                    std::snprintf(name, sizeof(name), "%s", shortName);
                }
            }
            TextFit(dl, Txt::Strong, ImVec2(x0 + U(18), ny), colW - U(18), C::Text, name);
        } else {
            Text(dl, Txt::Body, ImVec2(x0, ny), C::Text3, Tr("Não definido"));
        }
    };
    if (rolesKnown && g_controller.DetectedIncidentKind() == IncidentKind::Dive) {
        // A simulation: the diver is the one to punish, the opponent fouled nobody and was fouled by nobody
        person(namesL, Tr("SIMULAÇÃO"), C::RoleOffender, offSlot);
        person(namesL + colW + arrowW, Tr("ADVERSÁRIO"), C::Text2, vicSlot);
    } else if (rolesKnown) {
        person(namesL, Tr("INFRATOR"), C::RoleOffender, offSlot);
        IconChevron(dl, ImVec2(namesL + colW + arrowW * 0.5f, y2 + LineH(Txt::Caption) + U(4) + nameH * 0.5f), U(10), Dir::Right, C::Text3);
        person(namesL + colW + arrowW, Tr("VÍTIMA"), C::RoleFouled, vicSlot);
    } else {
        // No proven contact: the two players of the duel, without saying who fouled whom
        person(namesL, Tr("DISPUTA"), C::Text2, offSlot);
        person(namesL + colW + arrowW, Tr("DISPUTA"), C::Text2, vicSlot);
    }

    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(contentW, h));
}

// ============================================================================
// Tactical pitch
// ============================================================================
void RenderPitch(ImDrawList* dl, float contentW, float radarH, VarState& state) {
    const auto& tel = g_controller.Telemetry();
    const auto& players = g_controller.Players();
    const auto& teams = g_controller.Teams();
    const PitchDims& d = tel.pitchDims;

    static int32_t s_lockPhase = -999;
    static float s_lockTime = -1e9f;
    static ReplaySyncResult s_lockSync{};
    static uint32_t s_lockRev = 0;
    static uint32_t s_lockSnapRev = 0;
    const ReplaySyncResult* syncPtr = &g_controller.Sync();
    int32_t lockPhase = 0;
    float lockTime = 0.0f;
    if (g_controller.GetLockedMarkerTime(lockPhase, lockTime) && std::fabs(tel.replayCursorTime - lockTime) > 6.0f) {
        if (s_lockPhase != lockPhase || std::fabs(s_lockTime - lockTime) > 0.05f || s_lockRev != g_frameDataRevision ||
            s_lockSnapRev != g_frameSnapRevision) {
            s_lockSync = state.SyncToReplayCursor(lockPhase, lockTime);
            s_lockPhase = lockPhase;
            s_lockTime = lockTime;
            s_lockRev = g_frameDataRevision;
            s_lockSnapRev = g_frameSnapRevision;
        }
        if (s_lockSync.hasSnapshot) syncPtr = &s_lockSync;
    }
    const auto& sync = *syncPtr;

    const Vec3 spot = g_controller.EffectivePreviewSpot();
    const float radarAspect = contentW / std::max(60.0f, radarH);
    const PitchRadarViewport vpTarget = PitchRadarViewport::Compute(d, spot, g_controller.Config().autoZoomBox,
                                                                    g_controller.RadarZoomOverride(), radarAspect);
    // The framing glides to its target (about 150 ms) instead of cutting: zoom changes and the play view
    // following the play read as camera moves. A new panel size snaps.
    static PitchRadarViewport s_vpShown{};
    static float s_vpAspect = -1.0f;
    if (std::fabs(radarAspect - s_vpAspect) > 0.01f) {
        s_vpShown = vpTarget;
        s_vpAspect = radarAspect;
    } else {
        const float k = 1.0f - std::exp(-std::max(0.0f, ImGui::GetIO().DeltaTime) * 20.0f);
        s_vpShown.minX += (vpTarget.minX - s_vpShown.minX) * k;
        s_vpShown.maxX += (vpTarget.maxX - s_vpShown.maxX) * k;
        s_vpShown.minZ += (vpTarget.minZ - s_vpShown.minZ) * k;
        s_vpShown.maxZ += (vpTarget.maxZ - s_vpShown.maxZ) * k;
    }
    PitchRadarViewport vp = s_vpShown;
    vp.isZoomed = vpTarget.isZoomed;
    vp.isPlayView = vpTarget.isPlayView;
    vp.zoomSide = vpTarget.zoomSide;
    // Token and ball sizes follow the real scale (pixels per metre), with readable minimums in pixels
    const float pxPerM = contentW / std::max(1.0f, vp.maxX - vp.minX);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 end(pos.x + contentW, pos.y + radarH);

    ImGui::InvisibleButton("##pitch_radar", ImVec2(contentW, radarH));
    const bool radarHovered = ImGui::IsItemHovered();
    const bool radarClicked = ImGui::IsItemClicked(0);
    const bool radarDragging = (g_controller.Page() == HudPage::Configure) && ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 2.0f);

    const char* zoomTxt = vp.isZoomed ? Tr("ÁREA") : vp.isPlayView ? Tr("LANCE") : Tr("CAMPO");
    const float zoomW = TrackedW(zoomTxt) + U(16);
    const float zoomH = U(22);
    const ImVec2 zoomP(end.x - zoomW - U(8), end.y - zoomH - U(8));
    const ImVec2 zoomE(zoomP.x + zoomW, zoomP.y + zoomH);
    const bool overZoom = ImGui::IsMouseHoveringRect(zoomP, zoomE, false);

    if (radarClicked) {
        if (overZoom) {
            g_controller.ToggleRadarZoom();
        } else {
            const ImVec2 mp = ImGui::GetIO().MousePos;
            g_controller.SetDraftCustomSpot(vp.NormToWorld((mp.x - pos.x) / std::max(1.0f, contentW), (mp.y - pos.y) / std::max(1.0f, radarH), d));
        }
    } else if (radarDragging && !overZoom) {
        const ImVec2 mp = ImGui::GetIO().MousePos;
        g_controller.SetDraftCustomSpot(vp.NormToWorld((mp.x - pos.x) / std::max(1.0f, contentW), (mp.y - pos.y) / std::max(1.0f, radarH), d));
    }

    auto toScreen = [&](float wx, float wz) -> ImVec2 {
        float u = 0.0f, v = 0.0f;
        vp.WorldToNorm(wx, wz, u, v);
        return ImVec2(pos.x + u * contentW, pos.y + v * radarH);
    };
    const float lineTh = std::max(1.0f, Uf(1.2f));

    dl->AddRectFilled(pos, end, C::Surround, U(kRadCard));
    dl->PushClipRect(pos, end, true);
    const ImVec2 pTL = toScreen(-d.halfLength, -d.halfWidth);
    const ImVec2 pBR = toScreen(d.halfLength, d.halfWidth);
    const ImVec2 gTL(std::max(pos.x, pTL.x), std::max(pos.y, pTL.y));
    const ImVec2 gBR(std::min(end.x, pBR.x), std::min(end.y, pBR.y));
    dl->AddRectFilled(gTL, gBR, C::GrassA);
    constexpr int kStripes = 12;
    const float stripeW = (d.halfLength * 2.0f) / static_cast<float>(kStripes);
    for (int i = 0; i < kStripes; i += 2) {
        const ImVec2 a = toScreen(-d.halfLength + i * stripeW, -d.halfWidth);
        const ImVec2 b = toScreen(-d.halfLength + (i + 1) * stripeW, d.halfWidth);
        dl->AddRectFilled(ImVec2(std::max(gTL.x, a.x), gTL.y), ImVec2(std::min(gBR.x, b.x), gBR.y), C::GrassB);
    }

    dl->AddRect(pTL, pBR, C::Line, 0.0f, 0, lineTh);
    dl->AddLine(toScreen(0.0f, -d.halfWidth), toScreen(0.0f, d.halfWidth), C::Line, lineTh);
    const ImVec2 cMid = toScreen(0.0f, 0.0f);
    const float cR = std::fabs(toScreen(9.15f, 0.0f).x - cMid.x);
    if (cR > 3.0f) dl->AddCircle(cMid, cR, C::Line, 40, lineTh);
    dl->AddCircleFilled(cMid, Uf(2.0f), C::Line, 10);
    for (int side : { -1, 1 }) {
        const float s = static_cast<float>(side);
        const ImVec2 a18 = toScreen(s * d.halfLength, -d.box18Z), b18 = toScreen(s * d.box18X, d.box18Z);
        dl->AddRect(ImVec2(std::min(a18.x, b18.x), std::min(a18.y, b18.y)), ImVec2(std::max(a18.x, b18.x), std::max(a18.y, b18.y)), C::Line, 0.0f, 0, lineTh);
        const ImVec2 a6 = toScreen(s * d.halfLength, -d.box6Z), b6 = toScreen(s * d.box6X, d.box6Z);
        dl->AddRect(ImVec2(std::min(a6.x, b6.x), std::min(a6.y, b6.y)), ImVec2(std::max(a6.x, b6.x), std::max(a6.y, b6.y)), C::Line, 0.0f, 0, lineTh);
        const float dxBox = std::clamp((d.penSpotX - d.box18X) / 9.15f, 0.0f, 0.99f);
        const float maxAng = std::acos(dxBox);
        ImVec2 arc[17];
        for (int k = 0; k <= 16; ++k) {
            const float a = -maxAng + 2.0f * maxAng * (static_cast<float>(k) / 16.0f);
            arc[k] = toScreen(s * (d.penSpotX - 9.15f * std::cos(a)), 9.15f * std::sin(a));
        }
        dl->AddPolyline(arc, 17, C::Line, ImDrawFlags_None, lineTh);
        dl->AddLine(toScreen(s * d.halfLength, -3.66f), toScreen(s * d.halfLength, 3.66f), IM_COL32(255, 255, 255, 220), std::max(2.0f, Uf(3.0f)));
        dl->AddCircleFilled(toScreen(s * d.penSpotX, 0.0f), Uf(2.0f), C::Line, 10);
    }

    const bool configure = (g_controller.Page() == HudPage::Configure);
    const int32_t activeOff = configure ? g_controller.DraftOffenderSlot() : g_controller.DetectedOffenderSlot();
    const int32_t activeVic = configure ? g_controller.DraftVictimSlot() : g_controller.DetectedVictimSlot();
    // On the decision page the user names the offender; elsewhere roles only for a call or a proven contact
    const bool rolesKnown = configure || g_controller.DetectedRolesConfirmed();

    float offsideLineX = -1.0f; // screen x of the offside line when drawn
    if (sync.hasSnapshot && std::fabs(spot.x) >= 22.0f) {
        const int defTeam = ((spot.x >= 0.0f) == (tel.teamAttackDir[0] >= 0.0f)) ? 0 : 1;
        float ox = 0.0f;
        if (VarHudController::ComputeSecondLastDefenderX(sync.snapshot, defTeam, tel.teamAttackDir[defTeam], ox)) {
            DrawDashedLine(dl, toScreen(ox, -d.halfWidth), toScreen(ox, d.halfWidth), C::Offside, lineTh, U(5), U(4));
            offsideLineX = toScreen(ox, -d.halfWidth).x; // its label is placed after the players and tags
        }
    }


    // Team chips (which team defends each goal), under the players so they never hide a token or a tag
    {   // also in the zoomed view: which goal belongs to whom
        const int leftDef = (tel.teamAttackDir[0] < 0.0f) ? 0 : 1;
        auto chip = [&](bool left, int teamIdx) {
            const char* sn = g_controller.TeamShortName(teamIdx);
            const float maxTw = std::max(U(24), (end.x - pos.x) * 0.5f - U(16) - U(27));
            const float tw = std::min(TrackedW(sn), maxTw);
            const float cw = U(8) + U(8) + tw + U(6) + U(10) + U(6);
            const float ch = U(20);
            const ImVec2 a(left ? pos.x + U(8) : end.x - U(8) - cw, pos.y + U(8));
            dl->AddRectFilled(a, ImVec2(a.x + cw, a.y + ch), IM_COL32(8, 12, 18, 190), U(kRadChip));
            dl->AddCircleFilled(ImVec2(a.x + U(8) + U(3), a.y + ch * 0.5f), U(4), RgbToImU32(teams[teamIdx].primaryColor), 12);
            TrackedFit(dl, ImVec2(a.x + U(19), a.y + (ch - LineH(Txt::Caption)) * 0.5f), C::Text, sn, maxTw);
            // The team defending the left goal attacks to the right, and the other way round
            IconChevron(dl, ImVec2(a.x + cw - U(6) - U(5), a.y + ch * 0.5f), U(9), left ? Dir::Right : Dir::Left, C::Text2);
        };
        chip(true, leftDef);
        chip(false, 1 - leftDef);
    }

    // Motion history of the frame on screen (trails, headings), refreshed only when that frame changes
    static int32_t s_motPhase = -999;
    static float s_motTime = -1e9f;
    static uint32_t s_motRev = 0;
    static MotionSample s_motion[VarState::kMotionSamples];
    if (sync.hasSnapshot &&
        (s_motPhase != sync.snapshot.matchPhase || std::fabs(s_motTime - sync.snapshot.time60Hz) > 0.01f ||
         s_motRev != g_frameSnapRevision)) {
        state.SampleMotionHistory(sync.snapshot.matchPhase, sync.snapshot.time60Hz, s_motion);
        s_motPhase = sync.snapshot.matchPhase;
        s_motTime = sync.snapshot.time60Hz;
        s_motRev = g_frameSnapRevision;
    }

    const Vec3 ball = sync.hasSnapshot ? sync.snapshot.ballPos : tel.liveBallPos;
    const ImVec2 bp = toScreen(ball.x, ball.z);
    const float br = std::clamp(pxPerM * 0.5f, std::max(Uf(3.6f), 3.0f), std::max(Uf(5.2f), 4.0f));
    if (sync.hasSnapshot) {
        const PlaySnapshot& sn = sync.snapshot;
        const bool focusPlay = IsValidSlot(activeOff) || IsValidSlot(activeVic);
        // Minimum sizes in pixels: below them a shirt number is not readable (720p, 4:3, 800x600)
        const float rBase = std::clamp(pxPerM * 1.2f, std::max(Uf(5.8f), 6.5f), std::max(Uf(10.0f), 8.0f));
        const float rHero = std::max(rBase * 1.17f, 7.5f);
        auto isHero = [&](int s) { return s == activeOff || s == activeVic; };
        // Shirt numbers come from the baked font closest in size above the target, so they are never blurred by a
        // strong downscale at small UI scales
        auto numberFont = [&](float r) -> ImFont* {
            const float want = r * 1.3f;
            ImFont* cands[3] = { F.caption, F.strong, F.title };
            ImFont* best = nullptr;
            for (ImFont* f : cands) {
                if (!f) continue;
                if (f->FontSize >= want * 0.95f && (!best || f->FontSize < best->FontSize)) best = f;
            }
            if (!best) {
                for (ImFont* f : cands) if (f && (!best || f->FontSize > best->FontSize)) best = f;
            }
            return best ? best : ImGui::GetFont();
        };
        ImFont* numFontBase = numberFont(rBase);
        ImFont* numFontHero = numberFont(rHero);
        auto teamTint = [&](int s, float a) {
            const RgbColor pc = teams[SlotToTeam(s)].primaryColor;
            // Lightened kit colour so trails and arrows read on the dark grass (dark kits more)
            const float l = ColorHelpers::RelativeLuma(pc);
            const float k = (l < 90.0f) ? 0.6f : 0.35f;
            return IM_COL32(static_cast<int>(pc.r + (255 - pc.r) * k), static_cast<int>(pc.g + (255 - pc.g) * k),
                            static_cast<int>(pc.b + (255 - pc.b) * k), static_cast<int>(255.0f * a));
        };

        // Screen position of every token. In a contact the two players of the play are often closer than two
        // tokens: they are spread apart on screen around their midpoint (never in the pitch data)
        const bool bothOn = IsValidSlot(activeOff) && IsValidSlot(activeVic) && sn.playerOnPitch[activeOff] && sn.playerOnPitch[activeVic];
        ImVec2 tokenPos[22];
        for (int s = 0; s < 22; ++s) tokenPos[s] = toScreen(sn.playerPos[s].x, sn.playerPos[s].z);
        if (bothOn) {
            ImVec2& a = tokenPos[activeOff];
            ImVec2& b = tokenPos[activeVic];
            const float minD = 2.0f * rHero + Uf(10.0f);
            float dx = b.x - a.x, dy = b.y - a.y;
            float dd = std::sqrt(dx * dx + dy * dy);
            if (dd < minD) {
                if (dd < 0.5f) { dx = 1.0f; dy = 0.0f; dd = 1.0f; } // same spot: side by side
                const ImVec2 mid((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
                const float h = minD * 0.5f;
                a = ImVec2(mid.x - dx / dd * h, mid.y - dy / dd * h);
                b = ImVec2(mid.x + dx / dd * h, mid.y + dy / dd * h);
            }
        }

        // 1. Trails (1.5 s): the two players of the play (solid, kit colour) and the ball (dotted white, so a
        // white kit's trail never reads as the ball's)
        auto trail = [&](int s, ImU32 col, float width) {
            // The whole trail moves with the token when it was spread apart, so its shape stays the real one
            const ImVec2 real = toScreen(sn.playerPos[s].x, sn.playerPos[s].z);
            const ImVec2 shift(tokenPos[s].x - real.x, tokenPos[s].y - real.y);
            ImVec2 prev = tokenPos[s];
            for (int k = 0; k < VarState::kMotionSamples; ++k) {
                const MotionSample& m = s_motion[k];
                if (!m.valid || !m.onPitch[s]) break;
                const ImVec2 w = toScreen(m.pos[s].x, m.pos[s].z);
                const ImVec2 p(w.x + shift.x, w.y + shift.y);
                const float fade = 1.0f - static_cast<float>(k) / static_cast<float>(VarState::kMotionSamples);
                dl->AddLine(prev, p, WithAlpha(col, 0.85f * fade), std::max(1.0f, width * (0.45f + 0.55f * fade)));
                prev = p;
            }
        };
        if (IsValidSlot(activeVic) && sn.playerOnPitch[activeVic]) trail(activeVic, teamTint(activeVic, 1.0f), Uf(3.6f));
        if (IsValidSlot(activeOff) && sn.playerOnPitch[activeOff]) trail(activeOff, teamTint(activeOff, 1.0f), Uf(3.6f));
        for (int k = 0; k < VarState::kMotionSamples; ++k) {
            const MotionSample& m = s_motion[k];
            if (!m.valid) break;
            const float fade = 1.0f - static_cast<float>(k) / static_cast<float>(VarState::kMotionSamples);
            dl->AddCircleFilled(toScreen(m.ball.x, m.ball.z), std::max(1.0f, Uf(1.8f) * (0.5f + 0.5f * fade)),
                                IM_COL32(255, 255, 255, static_cast<int>(230.0f * fade)), 0);
        }

        // 2. Heading (direction and speed over the last 0.3 s) where it tells the story: the two players of the
        // play, the ball carrier and anyone within 20 m of the ball. Everyone else stays quiet.
        constexpr int kHeadingSample = 1; // out[1] = 0.30 s earlier (real time)
        const MotionSample& past = s_motion[kHeadingSample];
        auto heading = [&](int s, float r, float a) {
            if (!past.valid || !past.onPitch[s] || sn.simFrame <= past.simFrame) return;
            const float dx = sn.playerPos[s].x - past.pos[s].x;
            const float dz = sn.playerPos[s].z - past.pos[s].z;
            const float dist = std::sqrt(dx * dx + dz * dz);
            const float speed = dist / (static_cast<float>(sn.simFrame - past.simFrame) / 60.0f); // m/s, real time
            if (!(speed >= 1.5f && speed <= 14.0f)) return; // standing, a teleport, or a non-finite read
            const ImVec2 c = tokenPos[s];
            const ImVec2 o = toScreen(sn.playerPos[s].x, sn.playerPos[s].z);
            const ImVec2 f = toScreen(sn.playerPos[s].x + dx / dist, sn.playerPos[s].z + dz / dist);
            ImVec2 dir(f.x - o.x, f.y - o.y);
            const float dl2 = std::sqrt(dir.x * dir.x + dir.y * dir.y);
            if (!(dl2 >= 1e-3f)) return;
            dir = ImVec2(dir.x / dl2, dir.y / dl2);
            const float len = std::clamp(speed * Uf(1.5f), Uf(5.0f), Uf(16.0f));
            const ImVec2 p0(c.x + dir.x * (r + Uf(2.0f)), c.y + dir.y * (r + Uf(2.0f)));
            const ImVec2 p1(p0.x + dir.x * len, p0.y + dir.y * len);
            const ImU32 col = teamTint(s, a);
            dl->AddLine(p0, p1, col, std::max(1.4f, Uf(2.0f)));
            const float hs = Uf(4.0f);
            const ImVec2 nrm(-dir.y, dir.x);
            dl->AddTriangleFilled(ImVec2(p1.x + dir.x * hs, p1.y + dir.y * hs),
                                  ImVec2(p1.x + nrm.x * hs * 0.75f, p1.y + nrm.y * hs * 0.75f),
                                  ImVec2(p1.x - nrm.x * hs * 0.75f, p1.y - nrm.y * hs * 0.75f), col);
        };
        auto nearBall = [&](int s) {
            const float dx = sn.playerPos[s].x - ball.x, dz = sn.playerPos[s].z - ball.z;
            return dx * dx + dz * dz < 20.0f * 20.0f;
        };

        // Ball over its carrier's token would hide the shirt number: draw it on the token's edge, toward where it is
        ImVec2 ballDraw = bp;
        if (IsValidSlot(sn.ballOwnerSlot) && sn.playerOnPitch[sn.ballOwnerSlot]) {
            const ImVec2 tc = tokenPos[sn.ballOwnerSlot];
            const float rOwner = isHero(sn.ballOwnerSlot) ? rHero : rBase;
            float dx = bp.x - tc.x, dy = bp.y - tc.y;
            const float dd = std::sqrt(dx * dx + dy * dy);
            if (dd < rOwner + br * 0.5f) {
                if (dd < 0.5f) { dx = 0.7071f; dy = 0.7071f; } else { dx /= dd; dy /= dd; }
                ballDraw = ImVec2(tc.x + dx * (rOwner + br * 0.6f), tc.y + dy * (rOwner + br * 0.6f));
            }
        }

        // 3. Everyone else: slightly dimmed while a play is in focus, so the eye goes to the contact
        const float dim = focusPlay ? 0.72f : 1.0f;
        for (int s = 0; s < 22; ++s) {
            if (!sn.playerOnPitch[s] || isHero(s)) continue;
            if (s == sn.ballOwnerSlot || nearBall(s)) heading(s, rBase, 0.8f * dim);
            PitchToken(dl, numFontBase, tokenPos[s], rBase, teams[SlotToTeam(s)], players[s].shirtNumber, dim);
            if (s == sn.ballOwnerSlot) dl->AddCircle(tokenPos[s], rBase + Uf(4.0f), IM_COL32(255, 255, 255, 200), 0, std::max(1.0f, Uf(1.3f)));
        }

        // 4. The play: contact link and the two players with a halo
        if (bothOn) {
            DrawDashedLine(dl, tokenPos[activeOff], tokenPos[activeVic], IM_COL32(255, 255, 255, 200), std::max(1.2f, Uf(1.6f)), U(4), U(3));
        }
        auto hero = [&](int32_t slot, ImU32 ring) {
            if (!IsValidSlot(slot) || !sn.playerOnPitch[slot]) return;
            const ImVec2 pp = tokenPos[slot];
            heading(slot, rHero, 0.95f);
            dl->AddCircleFilled(pp, rHero + Uf(6.0f), WithAlpha(ring, 0.20f), 0);
            dl->AddCircle(pp, rHero + Uf(4.2f), ring, 0, std::max(1.6f, Uf(2.2f)));
            PitchToken(dl, numFontHero, pp, rHero, teams[SlotToTeam(slot)], players[slot].shirtNumber, 1.0f);
        };
        hero(activeVic, rolesKnown ? C::RoleFouled : C::Text2);
        hero(activeOff, rolesKnown ? C::RoleOffender : C::Text2);

        // 5. Ball: soft glow, white body, dark rim
        dl->AddCircleFilled(ballDraw, br + Uf(4.0f), IM_COL32(255, 255, 255, 40), 0);
        dl->AddCircleFilled(ballDraw, br, IM_COL32(255, 255, 255, 255), 0);
        dl->AddCircle(ballDraw, br, IM_COL32(10, 14, 20, 235), 0, std::max(1.0f, Uf(1.3f)));

        // 6. Role tags last, placed where they cover the fewest tokens, the ball and each other. A player outside
        // the visible part of the pitch gets no tag (it would point at nothing).
        ImVec2 tagMin[2], tagMax[2];
        int tagCount = 0;
        if (vp.isZoomed || vp.isPlayView || bothOn) {
            ImVec2 placedMin(0, 0), placedMax(0, 0);
            bool placed = false;
            auto overlap = [](ImVec2 a0, ImVec2 a1, ImVec2 b0, ImVec2 b1) {
                const float w = std::min(a1.x, b1.x) - std::max(a0.x, b0.x);
                const float h = std::min(a1.y, b1.y) - std::max(a0.y, b0.y);
                return (w > 0.0f && h > 0.0f) ? w * h : 0.0f;
            };
            static int s_tagPrev[2] = { -1, -1 };
            // The restart spot and its label (decision page) are drawn on top: tags keep clear of them
            const bool spotShown = IsNonZeroVec(spot) && g_controller.Page() == HudPage::Configure;
            const ImVec2 spotScr = toScreen(spot.x, spot.z);
            const ImVec2 spotMin(spotScr.x - U(36), spotScr.y - U(14));
            const ImVec2 spotMax(spotScr.x + U(36), spotScr.y + U(14) + LineH(Txt::Caption) + U(12));
            auto tag = [&](int role, int32_t slot, const char* text, ImU32 col, bool preferBelow) {
                if (!IsValidSlot(slot) || !sn.playerOnPitch[slot]) return;
                const ImVec2 pp = tokenPos[slot];
                if (pp.x < pos.x || pp.x > end.x || pp.y < pos.y || pp.y > end.y) return;
                const float tw = TrackedW(text) + U(10);
                const float th = LineH(Txt::Caption) + U(4);
                const float gap = rHero + Uf(7.0f);
                const ImVec2 cands[4] = {
                    ImVec2(pp.x - tw * 0.5f, preferBelow ? pp.y + gap : pp.y - gap - th),
                    ImVec2(pp.x - tw * 0.5f, preferBelow ? pp.y - gap - th : pp.y + gap),
                    ImVec2(pp.x + gap, pp.y - th * 0.5f),
                    ImVec2(pp.x - gap - tw, pp.y - th * 0.5f),
                };
                float bestCost = 1e30f;
                ImVec2 best = cands[0];
                int bestIdx = 0;
                float costs[4] = {};
                ImVec2 placedAt[4];
                for (int ci = 0; ci < 4; ++ci) {
                    ImVec2 a = cands[ci];
                    float cost = static_cast<float>(ci) * 4.0f; // keep the preferred order when costs tie
                    const float cx = std::clamp(a.x, pos.x + U(4), end.x - U(4) - tw);
                    const float cy = std::clamp(a.y, pos.y + U(4), end.y - U(4) - th);
                    cost += (std::fabs(cx - a.x) + std::fabs(cy - a.y)) * 40.0f; // had to be pushed inside
                    a = ImVec2(cx, cy);
                    const ImVec2 b(a.x + tw, a.y + th);
                    for (int s = 0; s < 22; ++s) {
                        if (!sn.playerOnPitch[s]) continue;
                        const float rr = (isHero(s) ? rHero + Uf(4.2f) : rBase + Uf(1.5f));
                        cost += overlap(a, b, ImVec2(tokenPos[s].x - rr, tokenPos[s].y - rr), ImVec2(tokenPos[s].x + rr, tokenPos[s].y + rr)) * (isHero(s) ? 6.0f : 1.0f);
                    }
                    cost += overlap(a, b, ImVec2(ballDraw.x - br, ballDraw.y - br), ImVec2(ballDraw.x + br, ballDraw.y + br)) * 6.0f;
                    if (placed) cost += overlap(a, b, placedMin, placedMax) * 20.0f;
                    if (spotShown) cost += overlap(a, b, spotMin, spotMax) * 20.0f;
                    costs[ci] = cost;
                    placedAt[ci] = a;
                    if (cost < bestCost) { bestCost = cost; best = a; bestIdx = ci; }
                }
                const int prev = s_tagPrev[role];
                if (prev >= 0 && prev < 4 && prev != bestIdx && costs[prev] <= bestCost * 1.25f + 40.0f) {
                    bestIdx = prev;
                    best = placedAt[prev];
                }
                s_tagPrev[role] = bestIdx;
                const ImVec2 bEnd(best.x + tw, best.y + th);
                dl->AddRectFilled(best, bEnd, IM_COL32(8, 12, 18, 225), U(kRadChip));
                EdgeAccent(dl, best, bEnd, col, U(kRadChip), U(3));
                Tracked(dl, ImVec2(best.x + U(6), best.y + U(2)), C::Text, text);
                placedMin = best;
                placedMax = bEnd;
                placed = true;
                if (tagCount < 2) { tagMin[tagCount] = best; tagMax[tagCount] = bEnd; ++tagCount; }
            };
            const bool vicBelow = !bothOn || sn.playerPos[activeVic].z >= sn.playerPos[activeOff].z;
            const bool dive = !configure && rolesKnown && g_controller.DetectedIncidentKind() == IncidentKind::Dive;
            tag(0, activeOff, dive ? Tr("SIMULAÇÃO") : rolesKnown ? Tr("INFRATOR") : Tr("DISPUTA"),
                rolesKnown ? C::RoleOffender : C::Text2, !vicBelow);
            tag(1, activeVic, dive ? Tr("ADVERSÁRIO") : rolesKnown ? Tr("VÍTIMA") : Tr("DISPUTA"),
                (rolesKnown && !dive) ? C::RoleFouled : C::Text2, vicBelow);
        }

        // 7. Offside label: beside the line (never across it), wherever it covers the fewest tokens, the ball and
        // the role tags; text opaque, only the line itself is translucent
        if (offsideLineX >= 0.0f) {
            const char* txt = Tr("IMPEDIMENTO");
            const float lw = TrackedW(txt) + U(10);
            const float lh = LineH(Txt::Caption) + U(4);
            const float top = pos.y + U(8) + U(20) + U(6);
            const float bottom = end.y - U(8) - U(22) - U(6) - lh;
            auto overlapArea = [](ImVec2 a0, ImVec2 a1, ImVec2 b0, ImVec2 b1) {
                const float w = std::min(a1.x, b1.x) - std::max(a0.x, b0.x);
                const float h = std::min(a1.y, b1.y) - std::max(a0.y, b0.y);
                return (w > 0.0f && h > 0.0f) ? w * h : 0.0f;
            };
            // The restart cross and its label (drawn later, on the decision page) keep the area above and below it
            const bool restartShown = IsNonZeroVec(spot) && g_controller.Page() == HudPage::Configure;
            ImVec2 rsMin(0, 0), rsMax(0, 0);
            if (restartShown) {
                const ImVec2 sp = toScreen(spot.x, spot.z);
                const float half = (TrackedW(Tr("COBRANÇA")) + U(10)) * 0.5f;
                const float reach = std::max(Uf(6.0f), 5.0f) + Uf(7.0f) + LineH(Txt::Caption) + U(4);
                rsMin = ImVec2(sp.x - half, sp.y - reach);
                rsMax = ImVec2(sp.x + half, sp.y + reach);
            }
            static int s_offPrev = -1;
            float bestCost = 1e30f;
            ImVec2 best(0, 0);
            bool found = false;
            int bestKey = -1;
            float prevCost = -1.0f;
            ImVec2 prevAt(0, 0);
            for (int side = 0; side < 2; ++side) {
                const float x = side == 0 ? offsideLineX + U(6) : offsideLineX - U(6) - lw;
                if (x < pos.x + U(4) || x + lw > end.x - U(4)) continue; // would cross the line once clamped
                for (int row = 0; row < 8 && bottom > top; ++row) {
                    const float y = top + (bottom - top) * static_cast<float>(row) / 7.0f;
                    const ImVec2 a(x, y), b(x + lw, y + lh);
                    float cost = static_cast<float>(side * 8 + row) * 0.5f;
                    for (int s = 0; s < 22; ++s) {
                        if (!sn.playerOnPitch[s]) continue;
                        const float rr = isHero(s) ? rHero + Uf(4.2f) : rBase + Uf(1.5f);
                        cost += overlapArea(a, b, ImVec2(tokenPos[s].x - rr, tokenPos[s].y - rr),
                                            ImVec2(tokenPos[s].x + rr, tokenPos[s].y + rr)) * (isHero(s) ? 6.0f : 1.0f);
                    }
                    cost += overlapArea(a, b, ImVec2(ballDraw.x - br, ballDraw.y - br), ImVec2(ballDraw.x + br, ballDraw.y + br)) * 6.0f;
                    for (int t = 0; t < tagCount; ++t) cost += overlapArea(a, b, tagMin[t], tagMax[t]) * 20.0f;
                    if (restartShown) cost += overlapArea(a, b, rsMin, rsMax) * 20.0f;
                    if (side * 8 + row == s_offPrev) { prevCost = cost; prevAt = a; }
                    if (cost < bestCost) { bestCost = cost; best = a; found = true; bestKey = side * 8 + row; }
                }
            }
            if (found && prevCost >= 0.0f && bestKey != s_offPrev && prevCost <= bestCost * 1.25f + 40.0f) {
                best = prevAt;
                bestKey = s_offPrev;
            }
            s_offPrev = bestKey;
            if (found) {
                dl->AddRectFilled(best, ImVec2(best.x + lw, best.y + lh), IM_COL32(8, 12, 18, 215), U(kRadChip));
                Tracked(dl, ImVec2(best.x + U(5), best.y + U(2)), C::OffsideText, txt);
            }
        }
    } else {
        // No recorded frame for this moment: the live ball only
        dl->AddCircleFilled(bp, br + Uf(4.0f), IM_COL32(255, 255, 255, 40), 0);
        dl->AddCircleFilled(bp, br, IM_COL32(255, 255, 255, 255), 0);
        dl->AddCircle(bp, br, IM_COL32(10, 14, 20, 235), 0, std::max(1.0f, Uf(1.3f)));
    }

    // Restart spot of the decision being edited: on top of everything, white (yellow means a card), labelled
    if (IsNonZeroVec(spot) && g_controller.Page() == HudPage::Configure) {
        const ImVec2 sp = toScreen(spot.x, spot.z);
        const float cr = std::max(Uf(6.0f), 5.0f);
        const float t = std::max(1.8f, Uf(2.2f));
        dl->AddCircleFilled(sp, cr + Uf(4.0f), IM_COL32(8, 12, 18, 150), 0);
        dl->AddCircle(sp, cr + Uf(2.5f), IM_COL32(255, 255, 255, 240), 0, t);
        dl->AddLine(ImVec2(sp.x - cr, sp.y - cr), ImVec2(sp.x + cr, sp.y + cr), IM_COL32(255, 255, 255, 255), t);
        dl->AddLine(ImVec2(sp.x + cr, sp.y - cr), ImVec2(sp.x - cr, sp.y + cr), IM_COL32(255, 255, 255, 255), t);
        const char* rt = Tr("COBRANÇA");
        const float rtw = TrackedW(rt) + U(10);
        const float rth = LineH(Txt::Caption) + U(4);
        const float rx = std::clamp(sp.x - rtw * 0.5f, pos.x + U(4), end.x - U(4) - rtw);
        // Below or above the cross, whichever covers fewer players (and stays inside the pitch)
        const float below = sp.y + cr + Uf(7.0f);
        const float above = sp.y - cr - Uf(7.0f) - rth;
        auto coverCost = [&](float y) {
            if (y < pos.y + U(4) || y + rth > end.y - U(4)) return 1e9f;
            float c = 0.0f;
            if (sync.hasSnapshot) {
                for (int s2 = 0; s2 < 22; ++s2) {
                    if (!sync.snapshot.playerOnPitch[s2]) continue;
                    const ImVec2 tp = toScreen(sync.snapshot.playerPos[s2].x, sync.snapshot.playerPos[s2].z);
                    const float rr = U(10);
                    const float ow = std::min(rx + rtw, tp.x + rr) - std::max(rx, tp.x - rr);
                    const float oh = std::min(y + rth, tp.y + rr) - std::max(y, tp.y - rr);
                    if (ow > 0.0f && oh > 0.0f) c += ow * oh;
                }
            }
            return c;
        };
        const float ry = (coverCost(above) < coverCost(below)) ? above : below;
        dl->AddRectFilled(ImVec2(rx, ry), ImVec2(rx + rtw, ry + rth), IM_COL32(8, 12, 18, 225), U(kRadChip));
        Tracked(dl, ImVec2(rx + U(5), ry + U(2)), C::Text, rt);
    }

    dl->AddRectFilled(zoomP, zoomE, overZoom ? IM_COL32(30, 38, 52, 230) : IM_COL32(8, 12, 18, 190), U(kRadChip));
    Tracked(dl, ImVec2(zoomP.x + U(8), zoomP.y + (zoomH - LineH(Txt::Caption)) * 0.5f), overZoom ? C::Text : C::Text2, zoomTxt);

    dl->PopClipRect();
    MaskOuterCorners(dl, pos, end, U(kRadCard), C::Panel); // the window background behind the card
    dl->AddRect(pos, end, radarHovered ? C::StrokeHi : C::Stroke, U(kRadCard), 0, 1.0f);
}

// ============================================================================
// Pages
// ============================================================================
void RenderReviewPage(ImDrawList* dl, float contentW, float radarH, const HudConfig& cfg, bool mouseMoved, VarState& state) {
    RenderTimeline(dl, contentW, state);
    ImGui::Dummy(ImVec2(0, U(10)));
    RenderIncidentCard(dl, contentW, state);
    if (cfg.showRadar) {
        ImGui::Dummy(ImVec2(0, U(10)));
        RenderPitch(dl, contentW, radarH, state);
    }

    SectionCaption(dl, "DECISÃO", U(14));
    const auto& opts = g_controller.MainOptions();
    const int sel = g_controller.SelectedIndex();
    const bool pad = InputManager::PreferPadGlyphs();
    const float rowH = U(40);
    for (int i = 0; i < static_cast<int>(opts.size()); ++i) {
        const auto& opt = opts[i];
        const ImVec2 rp = ImGui::GetCursorScreenPos();
        char id[24];
        std::snprintf(id, sizeof(id), "##opt_%d", i);
        bool hov = false;
        if (RowBase(dl, id, rp, ImVec2(contentW, rowH), i == sel, &hov)) g_controller.SelectMainOption(i, state);
        if (hov && mouseMoved && g_controller.Page() == g_pageAtFrameStart) g_controller.SetSelectedIndex(i);
        float rightW = 0.0f;
        if (i == sel) {
            const float gh = U(20);
            rightW = ActionGlyph(dl, ImVec2(0, 0), gh, cfg.bindings, BindAction::Confirm, pad, false);
            ActionGlyph(dl, ImVec2(rp.x + contentW - U(12) - rightW, rp.y + (rowH - gh) * 0.5f), gh, cfg.bindings, BindAction::Confirm, pad);
        }
        TextFit(dl, Txt::Strong, ImVec2(rp.x + U(16), rp.y + (rowH - LineH(Txt::Strong)) * 0.5f),
                contentW - U(16) - U(24) - rightW, opt.keepDecision ? C::Text2 : C::Text, Tr(opt.label));
        ImGui::SetCursorScreenPos(rp);
        ImGui::Dummy(ImVec2(contentW, rowH + U(4)));
    }
    if (sel >= 0 && sel < static_cast<int>(opts.size()) && opts[sel].sublabel && opts[sel].sublabel[0]) {
        const ImVec2 dp = ImGui::GetCursorScreenPos();
        TextFit(dl, Txt::Small, ImVec2(dp.x + U(2), dp.y + U(2)), contentW - U(4), C::Text3, Tr(opts[sel].sublabel));
        ImGui::Dummy(ImVec2(contentW, LineH(Txt::Small) + U(6)));
    }
}

void RenderSquadPage(ImDrawList* dl, float contentW, float maxPanelH, const VerdictPool& pool, bool mouseMoved, VarState& state) {
    const auto& teams = g_controller.Teams();
    const auto& players = g_controller.Players();
    const int team = g_controller.SquadTeamFilter();
    const int sel = g_controller.SelectedIndex();

    // Segmented team switch
    const float segH = U(38);
    const float gap = U(6);
    const float segW = (contentW - gap) * 0.5f;
    const ImVec2 s0 = ImGui::GetCursorScreenPos();
    for (int t = 0; t < 2; ++t) {
        const ImVec2 a(s0.x + t * (segW + gap), s0.y);
        const ImVec2 b(a.x + segW, a.y + segH);
        char id[16];
        std::snprintf(id, sizeof(id), "##sq_t%d", t);
        bool hov = false;
        if (HitArea(id, a, ImVec2(segW, segH), &hov)) g_controller.SelectSquadTeam(t);
        if (hov && mouseMoved && g_controller.Page() == g_pageAtFrameStart) g_controller.SetSelectedIndex(0);
        const bool active = (team == t);
        dl->AddRectFilled(a, b, active ? C::Selected : hov ? C::SurfaceHi : C::Surface, U(kRadCtl));
        if (active && sel == 0) dl->AddRect(a, b, C::Accent, U(kRadCtl), 0, std::max(1.2f, Uf(1.5f)));
        dl->AddRectFilled(ImVec2(a.x + U(10), a.y + U(11)), ImVec2(a.x + U(13), b.y - U(11)), RgbToImU32(teams[t].primaryColor), U(1.5f));
        TextFit(dl, Txt::Strong, ImVec2(a.x + U(22), a.y + (segH - LineH(Txt::Strong)) * 0.5f), segW - U(32),
                active ? C::Text : C::Text2, g_controller.TeamFullName(t));
    }
    ImGui::SetCursorScreenPos(s0);
    ImGui::Dummy(ImVec2(contentW, segH + U(8)));

    const float rowH = U(34);
    const float rowGap = U(2);
    const float natural = 11.0f * (rowH + rowGap);
    const float reserved = U(130);
    const float maxListH = std::max(U(140), maxPanelH - ImGui::GetCursorPosY() - reserved);
    const float listH = std::min(natural, maxListH);
    const bool scroll = natural > maxListH + 1.0f;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    if (ImGui::BeginChild("##squad_list", ImVec2(contentW, listH), ImGuiChildFlags_None, scroll ? 0 : ImGuiWindowFlags_NoScrollbar)) {
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        const float innerW = ImGui::GetContentRegionAvail().x;
        for (int i = 0; i < 11; ++i) {
            const int slot = team * 11 + i;
            const auto& pl = players[slot];
            const int rowIdx = i + 1;
            const bool isSel = (sel == rowIdx);
            const ImVec2 rp = ImGui::GetCursorScreenPos();
            char id[24];
            std::snprintf(id, sizeof(id), "##sq_p%d", slot);
            bool hov = false;
            if (RowBase(cdl, id, rp, ImVec2(innerW, rowH), isSel, &hov, false)) g_controller.SelectSquadPlayer(slot);
            if (hov && mouseMoved && g_controller.Page() == g_pageAtFrameStart) g_controller.SetSelectedIndex(rowIdx);
            if (!isSel && !hov && (i % 2 == 0)) cdl->AddRectFilled(rp, ImVec2(rp.x + innerW, rp.y + rowH), IM_COL32(255, 255, 255, 6), U(kRadCtl));

            const bool off = !pl.onPitch;
            const float ty = rp.y + (rowH - LineH(Txt::Body)) * 0.5f;
            char num[8];
            std::snprintf(num, sizeof(num), "%u", static_cast<unsigned>(pl.shirtNumber));
            const float numColW = U(30);
            if (pl.shirtNumber > 0) Text(cdl, Txt::Strong, ImVec2(rp.x + U(10) + numColW - Measure(Txt::Strong, num), ty), off ? C::Text3 : C::Text2, num);

            // Right side: cards, sent-off tag, and pending POOL badge
            float right = rp.x + innerW - U(12);
            const float chipW = U(9), chipH = U(13);
            const float cy = rp.y + rowH * 0.5f;
            auto card = [&](ImU32 c) {
                cdl->AddRectFilled(ImVec2(right - chipW, cy - chipH * 0.5f), ImVec2(right, cy + chipH * 0.5f), c, U(1.5f));
                right -= chipW + U(4);
            };
            if (pl.reds > 0) card(C::Red);
            for (int y = 0; y < std::min<int>(pl.yellows, 2); ++y) card(C::Yellow);
            if (off) {
                const char* tag = Tr("Expulso");
                const float tw = Measure(Txt::Small, tag);
                Text(cdl, Txt::Small, ImVec2(right - tw - U(2), rp.y + (rowH - LineH(Txt::Small)) * 0.5f), C::Red, tag);
                right -= tw + U(10);
            }
            if (pool.FindCardForSlot(slot)) {
                const char* pTag = Tr("POOL");
                const float ptw = TrackedW(pTag);
                const float pw = ptw + U(10);
                const float ph = U(18);
                const ImVec2 pa(right - pw, cy - ph * 0.5f);
                cdl->AddRectFilled(pa, ImVec2(pa.x + pw, pa.y + ph), C::AccentSoft, U(kRadChip));
                Tracked(cdl, ImVec2(pa.x + U(5), pa.y + (ph - LineH(Txt::Caption)) * 0.5f), C::AccentHi, pTag);
                right -= pw + U(8);
            }
            // Name (without the "#N" prefix: the number has its own column)
            char full[80];
            g_controller.FormatPlayerBroadcastBuf(slot, full, sizeof(full));
            const char* name = full;
            if (full[0] == '#') {
                const char* sp = std::strchr(full, ' ');
                if (sp) name = sp + 1;
            }
            const float nx = rp.x + U(10) + numColW + U(12);
            TextFit(cdl, isSel ? Txt::Strong : Txt::Body, ImVec2(nx, ty), std::max(U(40), right - nx - U(6)), off ? C::Text3 : C::Text, name);

            ImGui::SetCursorScreenPos(rp);
            ImGui::Dummy(ImVec2(innerW, rowH + rowGap));
            if (isSel && scroll && g_scrollToSelection) {
                ImGui::SetScrollHereY(0.5f);
                g_scrollToSelection = false;
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, U(6)));
    const ImVec2 bp = ImGui::GetCursorScreenPos();
    bool hov = false;
    if (RowBase(dl, "##sq_back", bp, ImVec2(contentW, U(36)), sel == 12, &hov)) g_controller.SelectTab(HudPage::Main);
    if (hov && mouseMoved && g_controller.Page() == g_pageAtFrameStart) g_controller.SetSelectedIndex(12);
    IconChevron(dl, ImVec2(bp.x + U(18), bp.y + U(18)), U(10), Dir::Left, C::Text2);
    Text(dl, Txt::Body, ImVec2(bp.x + U(32), bp.y + (U(36) - LineH(Txt::Body)) * 0.5f), C::Text2, Tr("Voltar"));
    ImGui::SetCursorScreenPos(bp);
    ImGui::Dummy(ImVec2(contentW, U(36)));
    (void)state;
}

void RenderOptionsPage(ImDrawList* dl, float contentW, const HudConfig& cfg, bool mouseMoved, VarState& state) {
    const int sel = g_controller.SelectedIndex();
    const float rowH = U(40);
    const float valueW = U(140);

    auto rowStart = [&](int idx, bool* hov) -> ImVec2 {
        const ImVec2 rp = ImGui::GetCursorScreenPos();
        char id[24];
        std::snprintf(id, sizeof(id), "##cfg_%d", idx);
        const bool clicked = RowBase(dl, id, rp, ImVec2(contentW, rowH), sel == idx, hov);
        if (*hov && mouseMoved && g_controller.Page() == g_pageAtFrameStart) g_controller.SetSelectedIndex(idx);
        if (clicked) g_controller.TriggerSettingsRow(idx, +1, state);
        return rp;
    };
    auto rowEnd = [&](ImVec2 rp) {
        ImGui::SetCursorScreenPos(rp);
        ImGui::Dummy(ImVec2(contentW, rowH + U(4)));
    };
    auto label = [&](ImVec2 rp, const char* pt, float maxW) {
        TextFit(dl, Txt::Body, ImVec2(rp.x + U(16), rp.y + (rowH - LineH(Txt::Body)) * 0.5f), maxW, C::Text, Tr(pt));
    };
    auto toggleRow = [&](int idx, const char* pt, bool on) {
        bool hov = false;
        const ImVec2 rp = rowStart(idx, &hov);
        label(rp, pt, contentW - U(80));
        Toggle(dl, ImVec2(rp.x + contentW - U(14), rp.y + rowH * 0.5f), on);
        rowEnd(rp);
    };
    auto selectorRow = [&](int idx, const char* pt, const char* value) {
        const ImVec2 rp = ImGui::GetCursorScreenPos();
        bool hov = false;
        char id[24];
        std::snprintf(id, sizeof(id), "##cfgs_%d", idx);
        RowBase(dl, id, rp, ImVec2(contentW, rowH), sel == idx, &hov, true, false);
        if (hov && mouseMoved && g_controller.Page() == g_pageAtFrameStart) g_controller.SetSelectedIndex(idx);
        label(rp, pt, contentW - valueW - U(80));
        int delta = ValueSelector(dl, id, rp, ImVec2(contentW, rowH), valueW, value, C::Text, sel == idx);
        {
            char lid[28];
            std::snprintf(lid, sizeof(lid), "##cfgl_%d", idx);
            const float labelW = std::max(U(20), contentW - valueW - U(8) - U(26) * 2.0f - U(4));
            if (HitArea(lid, rp, ImVec2(labelW, rowH), nullptr)) delta = +1;
        }
        if (delta != 0) g_controller.TriggerSettingsRow(idx, delta, state);
        rowEnd(rp);
    };

    SectionCaption(dl, "PAINEL", 0.0f);
    toggleRow(0, "Abrir no replay", cfg.autoOpenPanel);
    toggleRow(1, "Campo tático", cfg.showRadar);
    toggleRow(2, "Zoom automático na área", cfg.autoZoomBox);
    selectorRow(3, "Posição", cfg.dockLeft ? Tr("Esquerda") : Tr("Direita"));
    selectorRow(4, "Tamanho", VarHudController::ScalePresetLabel(cfg.scalePreset));
    SectionCaption(dl, "PARTIDA", U(10));
    toggleRow(5, "Alertas durante o jogo", cfg.broadcastAlerts);
    toggleRow(kSettingsRowReplayAfterDecision, "Replay do lance após a decisão", cfg.replayAfterDecision);
    SectionCaption(dl, "GERAL", U(10));
    selectorRow(kSettingsRowLanguage, "Idioma", i18n::LanguageDisplayName(cfg.language == 1 ? Language::English : Language::PtBr));
    toggleRow(kSettingsRowCheckUpdates, "Atualização automática", cfg.autoUpdate);
    {
        // Status under the toggle (not selectable). An update already installed is shown even with the option off.
        const UpdateChecker::Phase phase = UpdateChecker::CurrentPhase();
        const char* newer = UpdateChecker::NewerVersion();
        char line[96];
        const char* second = nullptr;
        ImU32 col = C::Text3;
        if (newer && phase == UpdateChecker::Phase::Updated) {
            std::snprintf(line, sizeof(line), Tr("Atualizado para a versão %s"), newer);
            second = UpdateChecker::kReleasesUrl;
            col = C::Green;
        } else if (newer && phase == UpdateChecker::Phase::Installed) {
            std::snprintf(line, sizeof(line), Tr("Versão %s instalada"), newer);
            second = Tr("Vale ao reiniciar o jogo");
            col = C::Green;
        } else if (newer && phase == UpdateChecker::Phase::Installing && cfg.autoUpdate) {
            std::snprintf(line, sizeof(line), Tr("Baixando a versão %s..."), newer);
            col = C::Accent;
        } else if (newer && cfg.autoUpdate) {
            std::snprintf(line, sizeof(line), Tr("Nova versão %s disponível em"), newer);
            second = UpdateChecker::kReleasesUrl;
            col = C::Accent;
        } else if (cfg.autoUpdate) {
            std::snprintf(line, sizeof(line), Tr("Versão instalada: %s"), VAR_MOD_VERSION);
        } else {
            line[0] = '\0';
        }
        if (line[0]) {
            const ImVec2 lp = ImGui::GetCursorScreenPos();
            const float lh = LineH(Txt::Small);
            TextFit(dl, Txt::Small, ImVec2(lp.x + U(16), lp.y), contentW - U(32), col, line);
            if (second) TextFit(dl, Txt::Small, ImVec2(lp.x + U(16), lp.y + lh + U(2)), contentW - U(32), C::Text2, second);
            ImGui::Dummy(ImVec2(contentW, (second ? lh * 2.0f + U(2) : lh) + U(6)));
        }
    }
    {
        bool hov = false;
        const ImVec2 rp = rowStart(kSettingsRowControls, &hov);
        label(rp, "Controles", contentW * 0.5f);
        const char* v = Tr("Teclado e controle");
        const float vw = Measure(Txt::Small, v);
        Text(dl, Txt::Small, ImVec2(rp.x + contentW - U(34) - vw, rp.y + (rowH - LineH(Txt::Small)) * 0.5f), C::Text3, v);
        IconChevron(dl, ImVec2(rp.x + contentW - U(18), rp.y + rowH * 0.5f), U(10), Dir::Right, C::Text2);
        rowEnd(rp);
    }
    ImGui::Dummy(ImVec2(0, U(6)));
    {
        bool hov = false;
        const ImVec2 rp = rowStart(kSettingsRowBack, &hov);
        IconChevron(dl, ImVec2(rp.x + U(18), rp.y + rowH * 0.5f), U(10), Dir::Left, C::Text2);
        Text(dl, Txt::Body, ImVec2(rp.x + U(32), rp.y + (rowH - LineH(Txt::Body)) * 0.5f), C::Text2, Tr("Voltar"));
        rowEnd(rp);
    }
}

void RenderControlsPage(ImDrawList* dl, float contentW, float maxPanelH, bool mouseMoved, VarState& state) {
    const HudConfig& cfg = g_controller.Config();
    const int sel = g_controller.SelectedIndex();
    const int selCol = g_controller.ControlsColumn();
    const bool capturing = g_controller.IsCapturingBinding();
    const uint64_t now = GetTickCount64();

    const float labelW = std::floor(contentW * 0.46f);
    const float gap = U(4);
    const float rowH = U(32);
    const float rowGap = U(2);
    const float gh = U(20);

    {
        const ImVec2 hp = ImGui::GetCursorScreenPos();
        const float innerW = contentW - U(8);
        const float cellW = (innerW - labelW - 3.0f * gap) / 3.0f;
        Tracked(dl, ImVec2(hp.x + U(12), hp.y), C::Text3, Tr("AÇÃO"));
        const char* heads[3] = { Tr("TECLA 1"), Tr("TECLA 2"), Tr("CONTROLE") };
        for (int c = 0; c < 3; ++c) {
            const float cx = hp.x + labelW + gap + c * (cellW + gap);
            Tracked(dl, ImVec2(cx + (cellW - TrackedW(heads[c])) * 0.5f, hp.y), (c == selCol) ? C::Accent : C::Text3, heads[c]);
        }
        ImGui::Dummy(ImVec2(contentW, LineH(Txt::Caption) + U(8)));
    }

    const float natural = kBindActionCount * (rowH + rowGap);
    const float reserved = U(190);
    const float maxListH = std::max(U(150), maxPanelH - ImGui::GetCursorPosY() - reserved);
    const float listH = std::min(natural, maxListH);
    const bool scroll = natural > maxListH + 1.0f;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    if (ImGui::BeginChild("##controls_list", ImVec2(contentW, listH), ImGuiChildFlags_None, scroll ? 0 : ImGuiWindowFlags_NoScrollbar)) {
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        const float innerW = ImGui::GetContentRegionAvail().x;
        const float cellW = (innerW - labelW - 3.0f * gap) / 3.0f;
        for (int a = 0; a < kBindActionCount; ++a) {
            const BindAction act = static_cast<BindAction>(a);
            const bool rowSel = (sel == a);
            const ImVec2 rp = ImGui::GetCursorScreenPos();
            const ImVec2 re(rp.x + innerW, rp.y + rowH);
            const bool rowHov = ImGui::IsMouseHoveringRect(rp, re, true);
            if (rowSel) {
                cdl->AddRectFilled(rp, re, C::Selected, U(kRadCtl));
                EdgeAccent(cdl, rp, re, C::Accent, U(kRadCtl), U(3));
            } else if (rowHov) {
                cdl->AddRectFilled(rp, re, C::SurfaceHi, U(kRadCtl));
            } else if (a % 2 == 0) {
                cdl->AddRectFilled(rp, re, IM_COL32(255, 255, 255, 6), U(kRadCtl));
            }
            TextFit(cdl, Txt::Body, ImVec2(rp.x + U(12), rp.y + (rowH - LineH(Txt::Body)) * 0.5f), labelW - U(16), C::Text,
                    VarHudController::BindActionLabel(a));

            for (int c = 0; c < 3; ++c) {
                const float cx = rp.x + labelW + gap + c * (cellW + gap);
                const ImVec2 cp(cx, rp.y + U(3));
                const ImVec2 ce(cx + cellW, re.y - U(3));
                char cid[32];
                std::snprintf(cid, sizeof(cid), "##bind_%d_%d", a, c);
                bool cellHov = false;
                const bool clicked = HitArea(cid, cp, ImVec2(cellW, ce.y - cp.y), &cellHov);
                if (cellHov && mouseMoved && !capturing) {
                    g_controller.SetSelectedIndex(a);
                    g_controller.SetControlsColumn(c);
                }
                if (clicked && !capturing) g_controller.BeginBindingCapture(a, c, state);

                const bool isCap = capturing && g_controller.CaptureRow() == a && g_controller.CaptureColumn() == c;
                const bool isSelCell = rowSel && selCol == c;
                if (isCap) {
                    const float pulse = 0.55f + 0.45f * std::sin(static_cast<float>(now % 1000ULL) * 0.00628f);
                    cdl->AddRectFilled(cp, ce, C::AccentSoft, U(kRadChip));
                    cdl->AddRect(cp, ce, WithAlpha(C::Accent, pulse), U(kRadChip), 0, std::max(1.4f, Uf(1.6f)));
                    const char* dots = "\xE2\x80\xA6";
                    Text(cdl, Txt::Strong, ImVec2(cx + (cellW - Measure(Txt::Strong, dots)) * 0.5f, rp.y + (rowH - LineH(Txt::Strong)) * 0.5f), C::Text, dots);
                    continue;
                }
                if (isSelCell) cdl->AddRect(cp, ce, C::Accent, U(kRadChip), 0, std::max(1.2f, Uf(1.5f)));
                else if (cellHov) cdl->AddRectFilled(cp, ce, IM_COL32(255, 255, 255, 10), U(kRadChip));

                float gw = 0.0f;
                if (c < 2) {
                    const uint8_t vk = cfg.bindings.keys[static_cast<size_t>(a)][static_cast<size_t>(c)];
                    gw = Keycap(cdl, ImVec2(0, 0), gh, vk, C::Text, false, cellW - U(4));
                    if (gw > 0.0f) Keycap(cdl, ImVec2(cx + (cellW - gw) * 0.5f, rp.y + (rowH - gh) * 0.5f), gh, vk, C::Text, true, cellW - U(4));
                } else if (!InputBindings::IsPadRemappable(act)) {
                    gw = 0.0f; // gamepad only opens / collapses the panel
                } else {
                    const uint16_t m = cfg.bindings.pad[static_cast<size_t>(a)];
                    gw = PadGlyph(cdl, ImVec2(0, 0), gh, m, false);
                    if (gw > 0.0f) PadGlyph(cdl, ImVec2(cx + (cellW - gw) * 0.5f, rp.y + (rowH - gh) * 0.5f), gh, m);
                }
                if (gw <= 0.0f) {
                    const char* dash = "-";
                    Text(cdl, Txt::Body, ImVec2(cx + (cellW - Measure(Txt::Body, dash)) * 0.5f, rp.y + (rowH - LineH(Txt::Body)) * 0.5f), C::Text3, dash);
                }
            }
            ImGui::SetCursorScreenPos(rp);
            ImGui::Dummy(ImVec2(innerW, rowH + rowGap));
            if (rowSel && scroll && g_scrollToSelection) {
                ImGui::SetScrollHereY(0.5f);
                g_scrollToSelection = false;
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0, U(8)));
    auto actionRow = [&](const char* id, int idx, bool reset, const char* pt) -> bool {
        const ImVec2 rp = ImGui::GetCursorScreenPos();
        bool hov = false;
        const bool clicked = RowBase(dl, id, rp, ImVec2(contentW, U(36)), sel == idx, &hov);
        if (hov && mouseMoved && !capturing) g_controller.SetSelectedIndex(idx);
        if (reset) IconReset(dl, ImVec2(rp.x + U(19), rp.y + U(18)), U(14), C::Text2);
        else IconChevron(dl, ImVec2(rp.x + U(18), rp.y + U(18)), U(10), Dir::Left, C::Text2);
        Text(dl, Txt::Body, ImVec2(rp.x + U(34), rp.y + (U(36) - LineH(Txt::Body)) * 0.5f), C::Text2, Tr(pt));
        ImGui::SetCursorScreenPos(rp);
        ImGui::Dummy(ImVec2(contentW, U(36) + U(4)));
        return clicked && !capturing;
    };
    if (actionRow("##ctl_restore", kControlsRowRestore, true, "Restaurar padrão")) g_controller.RestoreDefaultBindings(state);
    if (actionRow("##ctl_back", kControlsRowBack, false, "Voltar")) {
        g_controller.SelectTab(HudPage::Config);
        g_controller.SetSelectedIndex(kSettingsRowControls);
    }

    const ImVec2 mp = ImGui::GetCursorScreenPos();
    if (capturing) {
        const char* prompt = (g_controller.CaptureColumn() == 2) ? Tr("Pressione o novo botão") : Tr("Pressione a nova tecla");
        Text(dl, Txt::Strong, ImVec2(mp.x + U(2), mp.y + U(2)), C::Accent, prompt);
    } else if (const char* msg = g_controller.ControlsMessage()) {
        const ImU32 col = g_controller.ControlsMessageIsError() ? C::Red : C::Green;
        dl->AddCircleFilled(ImVec2(mp.x + U(6), mp.y + U(2) + LineH(Txt::Small) * 0.5f), U(3), col, 10);
        TextFit(dl, Txt::Small, ImVec2(mp.x + U(16), mp.y + U(2)), contentW - U(16), col, msg);
    }
    ImGui::Dummy(ImVec2(contentW, LineH(Txt::Strong) + U(6)));
}

void BackIconAdapter(ImDrawList* dl, ImVec2 c, float sz, ImU32 col) { IconChevron(dl, c, sz * 0.8f, Dir::Left, col); }

void RenderDecisionPage(ImDrawList* dl, float contentW, float radarH, const HudConfig& cfg, const VerdictPool& pool, bool mouseMoved, VarState& state) {
    const bool pad = InputManager::PreferPadGlyphs();
    // Title block with back button
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float bs = U(32);
        if (IconButton(dl, "##dec_back", p, bs, BackIconAdapter, Tr("Voltar"))) {
            g_controller.TriggerConfigField(ConfigField::Back, 1, state);
        }
        const float tx = p.x + bs + U(12);
        Tracked(dl, ImVec2(tx, p.y - Uf(1)), C::Text3, Tr("DECISÃO"));
        TextFit(dl, Txt::Title, ImVec2(tx, p.y + LineH(Txt::Caption) + U(1)), contentW - (tx - p.x), C::Text,
                g_controller.DraftTitle());
        ImGui::SetCursorScreenPos(p);
        ImGui::Dummy(ImVec2(contentW, std::max(bs, LineH(Txt::Caption) + LineH(Txt::Title) + U(2)) + U(12)));
    }

    const auto& fields = g_controller.ActiveConfigFields();
    bool hasSpot = false;
    for (ConfigField f : fields) if (f == ConfigField::Spot) hasSpot = true;
    if (cfg.showRadar && g_controller.DraftAction() != VarAction::EditCard) {
        RenderPitch(dl, contentW, radarH, state);
        if (hasSpot) {
            const ImVec2 hp = ImGui::GetCursorScreenPos();
            TextFit(dl, Txt::Small, ImVec2(hp.x + U(2), hp.y + U(6)), contentW, C::Text3, Tr("Clique no campo para mover o local da cobrança"));
            ImGui::Dummy(ImVec2(contentW, LineH(Txt::Small) + U(12)));
        } else {
            ImGui::Dummy(ImVec2(0, U(10)));
        }
    }

    bool updatingExistingInPool = false;
    bool replacingPlayInPool = false;
    if (g_controller.DraftAction() == VarAction::EditCard) {
        VarVerdict probe{};
        probe.action = VarAction::EditCard;
        probe.directCardEdit = g_controller.DraftDirectCardMode();
        probe.offenderSlot = g_controller.DraftOffenderSlot();
        probe.offenderRosterIdx = g_controller.CurrentRosterOf(g_controller.DraftOffenderSlot());
        probe.sourceEventId = g_controller.DraftSourceEventId();
        updatingExistingInPool = (pool.FindSameKey(probe) != nullptr);
    } else {
        updatingExistingInPool = pool.hasPlayDecision && pool.playDecision.action != VarAction::None;
        const bool samePlay = (g_controller.DraftSourceEventId() != 0 &&
                               pool.playDecision.sourceEventId == g_controller.DraftSourceEventId()) ||
                              (g_controller.DraftSourceEventId() == 0 && pool.playDecision.sourceEventId == 0 &&
                               pool.playDecision.armStoppage == g_controller.Telemetry().stoppageCount &&
                               pool.playDecision.playPhase == g_controller.Telemetry().replayCursorPhase &&
                               std::fabs(pool.playDecision.playTime60Hz - g_controller.Telemetry().replayCursorTime) <= 180.0f);
        // Cancel of the stopped-for foul and a foul on another play live together (nothing is replaced)
        const VarAction da = g_controller.DraftAction();
        auto carriesFoul = [](VarAction a) {
            return a == VarAction::ConvertToPenalty || a == VarAction::ConvertToFreeKick ||
                   a == VarAction::RetroactivePenalty || a == VarAction::RetroactiveFreeKick;
        };
        const bool draftLiveCancel = (da == VarAction::OverturnCancel) && g_controller.DraftTargetsLive();
        const bool poolLiveCancel = pool.playDecision.action == VarAction::OverturnCancel && pool.playDecision.targetsLiveSetPlay;
        const bool coexist = ((draftLiveCancel && carriesFoul(pool.playDecision.action)) ||
                              (poolLiveCancel && carriesFoul(da))) &&
                             pool.playDecision.sourceEventId != g_controller.DraftSourceEventId();
        if (coexist) updatingExistingInPool = false;
        replacingPlayInPool = updatingExistingInPool && !samePlay;
    }

    const int sel = g_controller.SelectedIndex();
    const float rowH = U(40);
    const float valueW = std::min(U(200), contentW * 0.56f);
    const auto& players = g_controller.Players();
    const auto& teams = g_controller.Teams();
    for (int i = 0; i < static_cast<int>(fields.size()); ++i) {
        const ConfigField f = fields[i];
        const bool isSel = (i == sel);
        const ImVec2 rp = ImGui::GetCursorScreenPos();
        char id[24];
        std::snprintf(id, sizeof(id), "##fld_%d", i);

        if (f == ConfigField::Confirm) {
            if (g_controller.IsPostState13RedRescindWarning()) {
                dl->AddCircleFilled(ImVec2(rp.x + U(6), rp.y + U(2) + LineH(Txt::Small) * 0.5f), U(3), C::Yellow, 10);
                TextFit(dl, Txt::Small, ImVec2(rp.x + U(16), rp.y + U(2)), contentW - U(16), C::Yellow,
                        Tr("Cartão removido. O jogador volta a campo na próxima parada."));
                ImGui::Dummy(ImVec2(contentW, LineH(Txt::Small) + U(10)));
            } else {
                ImGui::Dummy(ImVec2(0, U(6)));
            }
            const ImVec2 bp = ImGui::GetCursorScreenPos();
            const float bh = U(46);
            bool hov = false;
            if (HitArea(id, bp, ImVec2(contentW, bh), &hov)) g_controller.TriggerConfigField(ConfigField::Confirm, 1, state);
            if (hov && mouseMoved && g_controller.Page() == g_pageAtFrameStart) g_controller.SetSelectedIndex(i);
            dl->AddRectFilled(bp, ImVec2(bp.x + contentW, bp.y + bh), hov ? C::AccentHi : C::Accent, U(kRadCtl));
            if (isSel) dl->AddRect(ImVec2(bp.x - U(3), bp.y - U(3)), ImVec2(bp.x + contentW + U(3), bp.y + bh + U(3)),
                                   WithAlpha(C::Accent, 0.55f), U(kRadCtl) + U(3), 0, std::max(1.5f, Uf(2.0f)));
            const char* lbl = replacingPlayInPool ? Tr("Substituir decisão")
                            : updatingExistingInPool ? Tr("Atualizar decisão") : Tr("Confirmar decisão");
            const float lw = Measure(Txt::Strong, lbl);
            Text(dl, Txt::Strong, ImVec2(bp.x + (contentW - lw) * 0.5f, bp.y + (bh - LineH(Txt::Strong)) * 0.5f), IM_COL32(255, 255, 255, 255), lbl);
            const float gh = U(20);
            const float gw = ActionGlyph(dl, ImVec2(0, 0), gh, cfg.bindings, BindAction::Confirm, pad, false);
            ActionGlyph(dl, ImVec2(bp.x + contentW - U(12) - gw, bp.y + (bh - gh) * 0.5f), gh, cfg.bindings, BindAction::Confirm, pad,
                        true, IM_COL32(255, 255, 255, 235));
            ImGui::SetCursorScreenPos(bp);
            ImGui::Dummy(ImVec2(contentW, bh + U(6)));
            continue;
        }
        if (f == ConfigField::Back) {
            bool hov = false;
            if (RowBase(dl, id, rp, ImVec2(contentW, U(36)), isSel, &hov, false)) g_controller.TriggerConfigField(ConfigField::Back, 1, state);
            if (hov && mouseMoved && g_controller.Page() == g_pageAtFrameStart) g_controller.SetSelectedIndex(i);
            const char* lbl = Tr("Voltar");
            const float lw = Measure(Txt::Body, lbl);
            Text(dl, Txt::Body, ImVec2(rp.x + (contentW - lw) * 0.5f, rp.y + (U(36) - LineH(Txt::Body)) * 0.5f), isSel ? C::Text : C::Text2, lbl);
            ImGui::SetCursorScreenPos(rp);
            ImGui::Dummy(ImVec2(contentW, U(36) + U(4)));
            continue;
        }

        bool hov = false;
        RowBase(dl, id, rp, ImVec2(contentW, rowH), isSel, &hov, true, false);
        if (hov && mouseMoved && g_controller.Page() == g_pageAtFrameStart) g_controller.SetSelectedIndex(i);
        const char* lbl = "";
        const char* value = "";
        ImU32 valueCol = C::Text;
        RgbColor dotRgb{};
        RgbColor* dot = nullptr;
        char nameBuf[80];
        switch (f) {
            case ConfigField::Card:
                lbl = Tr("Cartão");
                value = g_controller.DraftDirectCardMode() ? VarHudController::DirectCardPresetLabel(g_controller.DraftDirectCardPreset())
                                                           : VarHudController::CardLabel(g_controller.DraftCard());
                valueCol = (g_controller.DraftCard() == CardKind::Red) ? C::Red : (g_controller.DraftCard() == CardKind::Yellow) ? C::Yellow : C::Text;
                break;
            case ConfigField::RestartType:
                lbl = Tr("Bola parada");
                value = VarHudController::ActionLabel(g_controller.DraftAction());
                break;
            case ConfigField::Team: {
                lbl = (g_controller.DraftAction() == VarAction::OverturnCancel) ? Tr("Posse para") : Tr("Cobrança para");
                const int32_t t = g_controller.DraftBenefitedTeam();
                value = g_controller.TeamFullName(t);
                dotRgb = teams[t == 1 ? 1 : 0].primaryColor;
                dot = &dotRgb;
                break;
            }
            case ConfigField::Offender: {
                // The player who commits the foul (or is carded, for a card edit): "Jogador" alone was ambiguous
                lbl = (g_controller.DraftAction() == VarAction::EditCard) ? Tr("Jogador")
                    : (g_controller.DraftAnnulGoal() == 1) ? Tr("Impedido")
                    : Tr("Infrator");
                const int32_t s = g_controller.DraftOffenderSlot();
                if (IsValidSlot(s)) {
                    g_controller.FormatPlayerBroadcastBuf(s, nameBuf, sizeof(nameBuf));
                } else {
                    std::snprintf(nameBuf, sizeof(nameBuf), "%s", Tr("Escolha o jogador"));
                }
                value = nameBuf;
                if (IsValidSlot(s)) {
                    dotRgb = teams[players[s].team == 1 ? 1 : 0].primaryColor;
                    dot = &dotRgb;
                }
                break;
            }
            case ConfigField::Spot: {
                const bool sideField = (g_controller.DraftAction() == VarAction::RestartCorner || g_controller.DraftAction() == VarAction::RestartGoalKick);
                lbl = sideField ? Tr("Lado") : Tr("Local");
                value = sideField ? (g_controller.DraftSpotPos().z >= 0.0f ? Tr("Direito") : Tr("Esquerdo"))
                                  : VarHudController::SpotPresetLabel(g_controller.DraftSpotPreset());
                break;
            }
            default: break;
        }
        TextFit(dl, Txt::Body, ImVec2(rp.x + U(16), rp.y + (rowH - LineH(Txt::Body)) * 0.5f), contentW - valueW - U(80), C::Text2, lbl);
        const int delta = ValueSelector(dl, id, rp, ImVec2(contentW, rowH), valueW, value, valueCol, isSel, dot);
        if (delta != 0) g_controller.TriggerConfigField(f, delta, state);
        ImGui::SetCursorScreenPos(rp);
        ImGui::Dummy(ImVec2(contentW, rowH + U(4)));
    }
}

// ============================================================================
// Open panel
// ============================================================================
void CollapseIconAdapter(ImDrawList* dl, ImVec2 c, float sz, ImU32 col) { IconMinimize(dl, c, sz, col); }
void CloseIconAdapter(ImDrawList* dl, ImVec2 c, float sz, ImU32 col) { IconClose(dl, c, sz, col); }

void RenderDecisionPool(ImDrawList* dl, float contentW, const VerdictPool& pool, const HudConfig& cfg, VarState& state) {
    const size_t total = pool.Count();
    if (total == 0) return;

    struct PoolRow {
        bool        isPlay = false;
        uint32_t    key    = 0; // card entry sequence (unique)
        uint32_t    order  = 0;
        ImU32       tagCol = C::Accent;
        const char* tag    = "";
        const char* text   = "";
    };
    std::array<PoolRow, kMaxCardPoolEntries + 1> rows{};
    // Current-language summaries, rebuilt only when the pool, the language or the names change
    static std::array<std::string, kMaxCardPoolEntries + 1> rowTexts{};
    static uint64_t s_rowsKey = ~0ULL;
    const uint64_t rowsKey = (static_cast<uint64_t>(pool.sequence) << 24) ^ (static_cast<uint64_t>(total) << 16) ^
                             (static_cast<uint64_t>(pool.CardCount()) << 8) ^
                             (static_cast<uint64_t>(i18n::GetLanguage() == Language::English ? 1 : 0) << 7) ^
                             (pool.hasPlayDecision ? 0x40ULL : 0ULL) ^ (g_frameNamesRevision * 0x9E3779B97F4A7C15ULL);
    const bool rebuildRows = (rowsKey != s_rowsKey);
    s_rowsKey = rowsKey;
    size_t rowCount = 0;

    if (pool.hasPlayDecision) {
        const auto& pv = pool.playDecision;
        const bool isOverturn = (pv.action == VarAction::OverturnCancel ||
                                 pv.action == VarAction::RestartCorner ||
                                 pv.action == VarAction::RestartGoalKick ||
                                 pv.action == VarAction::RestartThrowIn);
        const ImU32 col = (pv.targetCard == CardKind::Red) ? C::Red : isOverturn ? C::Yellow : C::Accent;
        if (rebuildRows) rowTexts[rowCount] = g_controller.FormatSummaryOf(pv);
        rows[rowCount] = PoolRow{ true, 0U, pool.playOrder, col, Tr("LANCE"), rowTexts[rowCount].c_str() };
        ++rowCount;
    }

    const size_t cardStart = rowCount;
    for (const auto& ce : pool.cardEntries) {
        if (!ce.active) continue;
        const auto& cv = ce.verdict;
        const bool hasRed = (cv.targetCard == CardKind::Red) || (cv.directCardEdit && cv.directReds > 0);
        const bool hasYel = (cv.targetCard == CardKind::Yellow) || (cv.directCardEdit && cv.directYellows > 0 && cv.directReds == 0);
        const ImU32 col = hasRed ? C::Red : hasYel ? C::Yellow : C::Neutral;
        if (rebuildRows) rowTexts[rowCount] = g_controller.FormatSummaryOf(cv);
        // A cancel kept next to a foul on another play only removes its card and marks the call as cancelled
        const char* tag = (cv.action == VarAction::OverturnCancel) ? Tr("ANULAÇÃO") : Tr("CARTÃO");
        rows[rowCount] = PoolRow{ false, cv.sequence, ce.order, col, tag, rowTexts[rowCount].c_str() };
        ++rowCount;
    }
    if (rowCount > cardStart + 1) {
        std::sort(rows.begin() + cardStart, rows.begin() + rowCount, [](const PoolRow& a, const PoolRow& b) {
            return a.order < b.order;
        });
    }

    const bool pad = InputManager::PreferPadGlyphs();
    const char* notice = g_controller.PoolNoticeMessage();
    const float padX = U(12);
    const float padY = U(10);
    const float hdrH = U(26);
    const float itemH = U(26);
    const float itemGap = U(3);
    const size_t visRows = std::min<size_t>(rowCount, 3);
    const float listH = static_cast<float>(visRows) * itemH + static_cast<float>(visRows > 0 ? visRows - 1 : 0) * itemGap;
    const float noticeH = notice ? (U(5) + LineH(Txt::Small)) : 0.0f;
    const float cardH = padY + hdrH + U(6) + listH + noticeH + padY;

    const ImVec2 bp = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(bp, ImVec2(bp.x + contentW, bp.y + cardH), IM_COL32(18, 40, 30, 255), U(kRadCard));
    dl->AddRect(bp, ImVec2(bp.x + contentW, bp.y + cardH), WithAlpha(C::Green, 0.35f), U(kRadCard), 0, 1.0f);
    EdgeAccent(dl, bp, ImVec2(bp.x + contentW, bp.y + cardH), C::Green, U(kRadCard), U(3));

    // Header: POOL DE DECISOES (N)  Aplicadas juntas ao sair do replay .... [Limpar Del]
    const char* clearLbl = (total > 1) ? Tr("Limpar") : Tr("Cancelar");
    const float gh = U(18);
    const float gw = ActionGlyph(dl, ImVec2(0, 0), gh, cfg.bindings, BindAction::CancelVerdict, pad, false);
    const float cbw = U(10) + Measure(Txt::Small, clearLbl) + (gw > 0.0f ? U(6) + gw : 0.0f) + U(8);
    const float cbh = U(26);
    const ImVec2 cb(bp.x + contentW - padX - cbw, bp.y + padY);
    bool clearHov = false;
    if (HitArea("##pool_clear", cb, ImVec2(cbw, cbh), &clearHov)) { g_controller.ClearPoolNotice(); state.CancelVerdict(); }
    dl->AddRectFilled(cb, ImVec2(cb.x + cbw, cb.y + cbh), clearHov ? IM_COL32(255, 255, 255, 32) : IM_COL32(255, 255, 255, 14), U(kRadCtl));
    Text(dl, Txt::Small, ImVec2(cb.x + U(10), cb.y + (cbh - LineH(Txt::Small)) * 0.5f), C::Text, clearLbl);
    if (gw > 0.0f) {
        ActionGlyph(dl, ImVec2(cb.x + cbw - U(8) - gw, cb.y + (cbh - gh) * 0.5f), gh, cfg.bindings, BindAction::CancelVerdict, pad);
    }
    if (clearHov) Tooltip(Tr("Limpar pool"));

    char hdrTitle[64];
    std::snprintf(hdrTitle, sizeof(hdrTitle), Tr("POOL DE DECISÕES (%zu)"), total);
    const float tx = bp.x + padX + U(2);
    const float hdrCy = bp.y + padY + hdrH * 0.5f;
    const float capW = TrackedW(hdrTitle);
    Tracked(dl, ImVec2(tx, hdrCy - LineH(Txt::Caption) * 0.5f), C::Green, hdrTitle);
    const char* sub = (total > 1) ? Tr("Aplicadas ao sair do replay") : Tr("Aplicada ao sair do replay");
    const float subX = tx + capW + U(8);
    const float subMaxW = cb.x - subX - U(8);
    if (subMaxW > U(40)) {
        TextFit(dl, Txt::Small, ImVec2(subX, hdrCy - LineH(Txt::Small) * 0.5f), subMaxW, C::Text2, sub);
    }

    // Items list (up to 3 visible without scroll, scrollable if > 3)
    const ImVec2 listPos(bp.x + padX, bp.y + padY + hdrH + U(6));
    const float listW = contentW - padX * 2.0f;
    const bool useScroll = (rowCount > 3);

    auto drawRow = [&](ImDrawList* rdl, const PoolRow& r, ImVec2 rp, float rw, int idx) {
        rdl->AddRectFilled(rp, ImVec2(rp.x + rw, rp.y + itemH), IM_COL32(10, 22, 18, 175), U(kRadChip));
        const float rcy = rp.y + itemH * 0.5f;

        // Category pill ([LANCE] / [CARTAO])
        const float tagTw = TrackedW(r.tag);
        const float tagW = tagTw + U(10);
        const float tagH = U(18);
        const ImVec2 tp(rp.x + U(6), rcy - tagH * 0.5f);
        rdl->AddRectFilled(tp, ImVec2(tp.x + tagW, tp.y + tagH), WithAlpha(r.tagCol, 0.22f), U(kRadChip));
        Tracked(rdl, ImVec2(tp.x + U(5), tp.y + (tagH - LineH(Txt::Caption)) * 0.5f), r.tagCol, r.tag);

        // Remove [X] button
        const float xb = U(22);
        const ImVec2 xp(rp.x + rw - U(4) - xb, rcy - xb * 0.5f);
        char xid[24];
        (void)idx;
        std::snprintf(xid, sizeof(xid), "##pool_rm_%s%u", r.isPlay ? "p" : "c", static_cast<unsigned>(r.isPlay ? 0U : r.key));
        if (IconButton(rdl, xid, xp, xb, CloseIconAdapter, Tr("Remover do pool"), false)) {
            g_controller.ClearPoolNotice();
            if (r.isPlay) state.RemovePlayFromPool();
            else state.RemoveCardPoolEntry(r.key);
        }

        // Summary
        const float sx = tp.x + tagW + U(8);
        const float sMaxW = std::max(U(30), xp.x - sx - U(6));
        TextFit(rdl, Txt::Strong, ImVec2(sx, rcy - LineH(Txt::Strong) * 0.5f), sMaxW, C::Text, r.text);
    };

    ImGui::SetCursorScreenPos(listPos);
    if (useScroll) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        if (ImGui::BeginChild("##pool_scroll", ImVec2(listW, listH), ImGuiChildFlags_None)) {
            ImDrawList* cdl = ImGui::GetWindowDrawList();
            const float innerW = ImGui::GetContentRegionAvail().x;
            for (size_t i = 0; i < rowCount; ++i) {
                const ImVec2 rp = ImGui::GetCursorScreenPos();
                drawRow(cdl, rows[i], rp, innerW, static_cast<int>(i));
                ImGui::SetCursorScreenPos(rp);
                ImGui::Dummy(ImVec2(innerW, itemH + (i + 1 < rowCount ? itemGap : 0.0f)));
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    } else {
        for (size_t i = 0; i < rowCount; ++i) {
            const ImVec2 rp(listPos.x, listPos.y + static_cast<float>(i) * (itemH + itemGap));
            drawRow(dl, rows[i], rp, listW, static_cast<int>(i));
        }
    }

    if (notice) {
        const float ny = listPos.y + listH + U(5);
        dl->AddCircleFilled(ImVec2(bp.x + padX + U(4), ny + LineH(Txt::Small) * 0.5f), U(3), C::Yellow, 10);
        TextFit(dl, Txt::Small, ImVec2(bp.x + padX + U(12), ny), listW - U(12), C::Yellow, notice);
    }

    ImGui::SetCursorScreenPos(bp);
    ImGui::Dummy(ImVec2(contentW, cardH + U(10)));
}

void RenderConfirmToast(float vw, float vh) {
    uint64_t age = 0;
    const char* msg = g_controller.ConfirmToast(GetTickCount64(), age);
    if (!msg) return;
    const float t = static_cast<float>(age);
    const float total = static_cast<float>(VarHudController::kConfirmToastMs);
    const float a = std::clamp(std::min(t / 160.0f, (total - t) / 380.0f), 0.0f, 1.0f);
    const float rise = (1.0f - std::min(t / 160.0f, 1.0f)) * U(10);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float textW = Measure(Txt::Strong, msg);
    const float h = U(40);
    const float iconW = U(22);
    const float w = std::min(vw - U(40), U(18) + iconW + U(10) + textW + U(20));
    const ImVec2 p(CenterOutsidePanel(vw, w), std::floor(vh * 0.80f + rise));
    const ImVec2 e(p.x + w, p.y + h);
    dl->AddRectFilled(ImVec2(p.x, p.y + U(3)), ImVec2(e.x, e.y + U(3)), WithAlpha(IM_COL32(0, 0, 0, 255), 0.35f * a), U(kRadCard));
    dl->AddRectFilled(p, e, WithAlpha(IM_COL32(16, 34, 26, 255), 0.97f * a), U(kRadCard));
    dl->AddRect(p, e, WithAlpha(C::Green, 0.60f * a), U(kRadCard), 0, 1.0f);
    // Check mark
    const ImVec2 c(p.x + U(18) + iconW * 0.5f, p.y + h * 0.5f);
    dl->AddCircleFilled(c, iconW * 0.5f, WithAlpha(C::Green, a), 20);
    const float k = iconW * 0.22f;
    dl->AddLine(ImVec2(c.x - k, c.y), ImVec2(c.x - k * 0.25f, c.y + k * 0.8f), WithAlpha(IM_COL32(10, 20, 14, 255), a), std::max(1.5f, Uf(2.2f)));
    dl->AddLine(ImVec2(c.x - k * 0.25f, c.y + k * 0.8f), ImVec2(c.x + k * 1.1f, c.y - k * 0.8f), WithAlpha(IM_COL32(10, 20, 14, 255), a), std::max(1.5f, Uf(2.2f)));
    TextFit(dl, Txt::Strong, ImVec2(p.x + U(18) + iconW + U(10), p.y + (h - LineH(Txt::Strong)) * 0.5f),
            w - (U(18) + iconW + U(10)) - U(14), WithAlpha(C::Text, a), msg);
}

void RenderOpenPanel(float vw, float vh, VarState& state, const VerdictPool& pool) {
    const HudConfig cfg = g_controller.Config();
    const auto& tel = g_controller.Telemetry();
    const auto& teams = g_controller.Teams();
    const float panelW = std::min(U(440), std::max(vw * 0.42f, 240.0f));
    const float margin = U(20);
    const float maxPanelH = std::max(340.0f, vh - 2.0f * margin);
    const float posX = cfg.dockLeft ? margin : (vw - margin - panelW);

    const ImGuiIO& io = ImGui::GetIO();
    const bool mouseMoved = (std::fabs(io.MouseDelta.x) > 0.5f || std::fabs(io.MouseDelta.y) > 0.5f);
    if (mouseMoved) InputManager::NotePointerActivity();

    ImGui::SetNextWindowPos(ImVec2(posX, margin), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(panelW, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(panelW, 0.0f), ImVec2(panelW, maxPanelH));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    g_panelX0 = posX;
    g_panelX1 = posX + panelW;
    g_panelDrawnFrame = g_hudFrame;
    if (ImGui::Begin("##VarPanelV5", nullptr, kFlags)) {
        const float contentW = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        PanelShadow(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), U(kRadPanel)); // background list: always behind the panel
        state.SetMouseOverPanel(ImGui::IsMouseHoveringRect(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), false));

        // 1. Header: [VAR] | HOME vs AWAY | clock ........ [collapse]
        {
            const ImVec2 hp = ImGui::GetCursorScreenPos();
            const float hh = U(32);
            float bw = 0.0f;
            VarBadge(dl, ImVec2(hp.x, hp.y + (hh - U(24)) * 0.5f), U(24), &bw);
            float x = hp.x + bw + U(14);
            const float ty = hp.y + (hh - LineH(Txt::Strong)) * 0.5f;
            const float barH = U(16);
            const float reservedR = Measure(Txt::Small, "vs") + U(16) + U(14) + Measure(Txt::Body, "00:00") + hh + U(30) + U(18);
            const float tagMaxW = std::max(U(40), (contentW - (x - hp.x) - reservedR) * 0.5f);
            auto teamTag = [&](int t, bool barLeft) {
                const char* sn = g_controller.TeamShortName(t);
                if (barLeft) {
                    dl->AddRectFilled(ImVec2(x, hp.y + (hh - barH) * 0.5f), ImVec2(x + U(3), hp.y + (hh + barH) * 0.5f), RgbToImU32(teams[t].primaryColor), U(1.5f));
                    x += U(9);
                }
                x += TextFit(dl, Txt::Strong, ImVec2(x, ty), tagMaxW, C::Text, sn);
                if (!barLeft) {
                    x += U(6);
                    dl->AddRectFilled(ImVec2(x, hp.y + (hh - barH) * 0.5f), ImVec2(x + U(3), hp.y + (hh + barH) * 0.5f), RgbToImU32(teams[t].primaryColor), U(1.5f));
                    x += U(3);
                }
            };
            teamTag(0, true);
            x += U(8);
            Text(dl, Txt::Small, ImVec2(x, hp.y + (hh - LineH(Txt::Small)) * 0.5f), C::Text3, "vs");
            x += Measure(Txt::Small, "vs") + U(8);
            teamTag(1, false);
            int cm = 0, cs = 0;
            PitchGeometry::FormatMatchClock(tel.replayCursorPhase, tel.replayCursorTime, cm, cs);
            char clk[16];
            std::snprintf(clk, sizeof(clk), "%02d:%02d", cm, cs);
            x += U(14);
            Text(dl, Txt::Body, ImVec2(x, hp.y + (hh - LineH(Txt::Body)) * 0.5f), C::Text2, clk);

            char tip[96], keys[64];
            InputManager::FormatActionHint(BindAction::ToggleFocus, keys, sizeof(keys));
            std::snprintf(tip, sizeof(tip), "%s  (%s)", Tr("Recolher painel"), keys);
            if (IconButton(dl, "##collapse", ImVec2(hp.x + contentW - hh, hp.y), hh, CollapseIconAdapter, tip, false)) {
                g_controller.CollapseToReplay();
            }
            ImGui::SetCursorScreenPos(hp);
            ImGui::Dummy(ImVec2(contentW, hh + U(10)));
        }

        const HudPage page = g_controller.Page();

        // 2. Tabs (hidden on the decision page, which has its own back button)
        if (page != HudPage::Configure) {
            const ImVec2 tp = ImGui::GetCursorScreenPos();
            const float th = U(34);
            const float tw = contentW / 3.0f;
            struct TabDef { const char* name; HudPage p; };
            static constexpr TabDef kTabs[3] = { { "REVISÃO", HudPage::Main }, { "CARTÕES", HudPage::Squad }, { "OPÇÕES", HudPage::Config } };
            dl->AddRectFilled(ImVec2(tp.x, std::floor(tp.y + th) - 1.0f), ImVec2(tp.x + contentW, std::floor(tp.y + th)), C::Hair);
            for (int i = 0; i < 3; ++i) {
                const bool active = (page == kTabs[i].p) || (page == HudPage::Controls && kTabs[i].p == HudPage::Config);
                const ImVec2 a(tp.x + i * tw, tp.y);
                char id[16];
                std::snprintf(id, sizeof(id), "##tab_%d", i);
                bool hov = false;
                if (HitArea(id, a, ImVec2(tw, th), &hov)) g_controller.SelectTab(kTabs[i].p);
                const char* name = Tr(kTabs[i].name);
                const float nw = TrackedW(name);
                const float nx = a.x + (tw - nw) * 0.5f;
                Tracked(dl, ImVec2(nx, tp.y + (th - LineH(Txt::Caption)) * 0.5f - U(1)), active ? C::Text : hov ? C::Text2 : C::Text3, name);
                if (active) dl->AddRectFilled(ImVec2(nx - U(6), tp.y + th - U(2)), ImVec2(nx + nw + U(6), tp.y + th), C::Accent, U(1));
            }
            ImGui::SetCursorScreenPos(tp);
            ImGui::Dummy(ImVec2(contentW, th + U(14)));
        }

        // 3. Decision Pool (composite decisions: max 1 play/restart + independent per-player cards)
        const size_t poolCount = pool.Count();
        if (poolCount > 0) {
            RenderDecisionPool(dl, contentW, pool, cfg, state);
        } else if (const char* notice = g_controller.PoolNoticeMessage()) {
            // With an empty pool its card is not drawn: show the notice on its own (e.g. a refused confirm)
            const ImVec2 np = ImGui::GetCursorScreenPos();
            const float nh = LineH(Txt::Small) + U(14);
            dl->AddRectFilled(np, ImVec2(np.x + contentW, np.y + nh), IM_COL32(48, 38, 12, 235), U(kRadCtl));
            EdgeAccent(dl, np, ImVec2(np.x + contentW, np.y + nh), C::Yellow, U(kRadCtl), U(3));
            TextFit(dl, Txt::Small, ImVec2(np.x + U(12), np.y + U(7)), contentW - U(20), C::Text, notice);
            ImGui::Dummy(ImVec2(contentW, nh + U(8)));
        }

        // Pitch height: true pitch aspect, capped so the panel always fits the screen alongside the Pool.
        // The rest of the page (rows, description, hint bar) is measured every frame, so the pitch shrinks to
        // exactly what is left instead of pushing the hint bar out of the window (720p, Large, full pool).
        static float s_restH[2] = { 0.0f, 0.0f }; // [0] REVISÃO, [1] decision page
        const int restKey = (page == HudPage::Configure) ? 1 : 0;
        const bool hasPitch = (page == HudPage::Main || page == HudPage::Configure);
        const float pitchScale = (page == HudPage::Configure) ? 0.9f : 1.0f;
        const float pitchBudget = (poolCount == 0) ? 0.34f : (poolCount == 1) ? 0.28f : 0.24f;
        float radarH = std::clamp(contentW / 1.56f, U(112), maxPanelH * pitchBudget);
        const float bodyTopY = ImGui::GetCursorPosY();
        if (hasPitch && s_restH[restKey] > 0.0f) {
            const float avail = (maxPanelH - ImGui::GetStyle().WindowPadding.y - bodyTopY - s_restH[restKey]) / pitchScale;
            radarH = std::max(U(72), std::min(radarH, avail));
        }

        // 4. Page body
        if (page == HudPage::Main) {
            RenderReviewPage(dl, contentW, radarH, cfg, mouseMoved, state);
        } else if (page == HudPage::Squad) {
            RenderSquadPage(dl, contentW, maxPanelH, pool, mouseMoved, state);
        } else if (page == HudPage::Config) {
            RenderOptionsPage(dl, contentW, cfg, mouseMoved, state);
        } else if (page == HudPage::Controls) {
            RenderControlsPage(dl, contentW, maxPanelH, mouseMoved, state);
        } else if (page == HudPage::Configure) {
            RenderDecisionPage(dl, contentW, radarH * pitchScale, cfg, pool, mouseMoved, state);
        }

        // 5. Context hints
        ImGui::Dummy(ImVec2(0, U(10)));
        using H = Hint;
        if (page == HudPage::Main) {
            const Hint hs[] = { { H::AxisV, BindAction::Up, 0, "Navegar" }, { H::AxisH, BindAction::Left, 0, "Lance" },
                                { H::Action, BindAction::PlayPause, 0, "Reproduzir" }, { H::Action, BindAction::ToggleFocus, 0, "Recolher" } };
            HintBar(dl, contentW, hs, 4);
        } else if (page == HudPage::Squad) {
            const Hint hs[] = { { H::AxisV, BindAction::Up, 0, "Navegar" }, { H::AxisH, BindAction::Left, 0, "Time" },
                                { H::Action, BindAction::Confirm, 0, "Escolher" }, { H::Action, BindAction::Back, 0, "Voltar" } };
            HintBar(dl, contentW, hs, 4);
        } else if (page == HudPage::Config) {
            const Hint hs[] = { { H::AxisV, BindAction::Up, 0, "Navegar" }, { H::AxisH, BindAction::Left, 0, "Ajustar" },
                                { H::Action, BindAction::Confirm, 0, "Alterar" }, { H::Action, BindAction::Back, 0, "Voltar" } };
            HintBar(dl, contentW, hs, 4);
        } else if (page == HudPage::Controls) {
            if (g_controller.IsCapturingBinding()) {
                const Hint hs[] = { { H::RawKey, BindAction::Back, VK_ESCAPE, "Cancelar" } };
                HintBar(dl, contentW, hs, 1);
            } else {
                const Hint hs[] = { { H::AxisH, BindAction::Left, 0, "Coluna" }, { H::Action, BindAction::Confirm, 0, "Alterar" },
                                    { H::Action, BindAction::CancelVerdict, 0, "Limpar" }, { H::Action, BindAction::Back, 0, "Voltar" } };
                HintBar(dl, contentW, hs, 4);
            }
        } else {
            const Hint hs[] = { { H::AxisV, BindAction::Up, 0, "Navegar" }, { H::AxisH, BindAction::Left, 0, "Ajustar" },
                                { H::Action, BindAction::Confirm, 0, "Confirmar" }, { H::Action, BindAction::Back, 0, "Voltar" } };
            HintBar(dl, contentW, hs, 4);
        }
        if (hasPitch) {
            s_restH[restKey] = std::max(0.0f, ImGui::GetCursorPosY() - bodyTopY - radarH * pitchScale);
        }
    }
    ImGui::End();
}

void ApplyTheme() {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = U(kRadPanel);
    st.WindowBorderSize = 1.0f;
    st.WindowPadding = ImVec2(U(16), U(14));
    st.ItemSpacing = ImVec2(U(8), 0.0f);
    st.ItemInnerSpacing = ImVec2(U(6), U(4));
    st.PopupRounding = U(kRadCtl);
    st.PopupBorderSize = 1.0f;
    st.ScrollbarSize = U(6);
    st.ScrollbarRounding = U(3);
    st.ChildRounding = 0.0f;
    st.ChildBorderSize = 0.0f;
    st.FrameRounding = U(kRadCtl);
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg]             = ImGui::ColorConvertU32ToFloat4(C::Panel);
    c[ImGuiCol_PopupBg]              = ImGui::ColorConvertU32ToFloat4(IM_COL32(20, 24, 32, 252));
    c[ImGuiCol_Border]               = ImGui::ColorConvertU32ToFloat4(C::Stroke);
    c[ImGuiCol_Text]                 = ImGui::ColorConvertU32ToFloat4(C::Text);
    c[ImGuiCol_TextDisabled]         = ImGui::ColorConvertU32ToFloat4(C::Text3);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]        = ImGui::ColorConvertU32ToFloat4(IM_COL32(255, 255, 255, 40));
    c[ImGuiCol_ScrollbarGrabHovered] = ImGui::ColorConvertU32ToFloat4(IM_COL32(255, 255, 255, 70));
    c[ImGuiCol_ScrollbarGrabActive]  = ImGui::ColorConvertU32ToFloat4(IM_COL32(255, 255, 255, 90));
}

} // namespace

// ============================================================================
// Public API
// ============================================================================
// The HUD keeps the same share of the screen at any resolution: 1.0 at 1920x1080, proportional to the height,
// and limited by the width so 4:3 / 5:4 screens and narrow windows never get a panel too wide for them
// (1440 px of width per 1080 px of height = 4:3). A floor keeps text readable on tiny backbuffers and a ceiling
// keeps the font atlas reasonable at 5K / 8K. The size preset (Pequeno / Médio / Grande) scales on top.
float VarHudRenderer::ComputeUiScale(float viewportW, float viewportH, int preset) {
    if (viewportW <= 0.0f || viewportH <= 0.0f) return 1.0f;
    float s = std::min(viewportH / 1080.0f, viewportW / 1440.0f);
    if (preset == 0) s *= 0.88f;
    else if (preset == 2) s *= 1.14f;
    s = std::clamp(s, kMinUiScale, kMaxUiScale);
    return std::round(s * 20.0f) / 20.0f;
}

bool VarHudRenderer::EnsureFonts(float viewportW, float viewportH) {
    const float s = ComputeUiScale(viewportW, viewportH, g_lastScalePreset);
    if (F.body && std::fabs(s - F.baked) < 0.01f) return false;

    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    F = FontSet{};

    wchar_t winDirW[MAX_PATH] = {};
    const UINT n = GetWindowsDirectoryW(winDirW, MAX_PATH);
    char winDir[MAX_PATH * 3] = {};
    if (n > 0 && n < MAX_PATH) WideCharToMultiByte(CP_UTF8, 0, winDirW, -1, winDir, sizeof(winDir), nullptr, nullptr);
    else std::snprintf(winDir, sizeof(winDir), "C:\\Windows");

    static const ImWchar kRanges[] = { 0x0020, 0x017F, 0x018F, 0x018F, 0x01A0, 0x01A1, 0x01AF, 0x01B0, 0x01CD, 0x01D4,
                                       0x0218, 0x021B, 0x0259, 0x0259, 0x2013, 0x2014, 0x2022, 0x2022, 0x2026, 0x2026, 0 };
    // Legibility floor for small screens (720p, 4:3, 800x600): the UI scale keeps the panel's share of the screen,
    // but text below these sizes stops being readable. Only small scales are affected.
    auto load = [&](const char* file, float px, float minPx) -> ImFont* {
        char path[MAX_PATH * 3];
        std::snprintf(path, sizeof(path), "%s\\Fonts\\%s", winDir, file);
        wchar_t wpath[MAX_PATH * 2];
        MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH * 2);
        if (GetFileAttributesW(wpath) == INVALID_FILE_ATTRIBUTES) return nullptr;
        ImFontConfig cfg;
        cfg.OversampleH = 1;
        cfg.OversampleV = 1;
        cfg.PixelSnapH = true;
        return io.Fonts->AddFontFromFileTTF(path, std::floor(std::max(px * s, minPx) + 0.5f), &cfg, kRanges);
    };
    F.body = load("segoeui.ttf", 16.5f, 11.5f);
    F.strong = load("seguisb.ttf", 16.5f, 11.5f);
    if (!F.strong) F.strong = load("segoeuib.ttf", 16.5f, 11.5f);
    F.title = load("segoeuib.ttf", 20.0f, 13.0f);
    F.sm = load("segoeui.ttf", 14.0f, 10.5f);
    F.caption = load("seguisb.ttf", 12.5f, 9.5f);
    if (!F.caption) F.caption = load("segoeuib.ttf", 12.5f, 9.5f);
    if (!F.body) {
        ImFontConfig cfg;
        cfg.SizePixels = std::floor(15.0f * s + 0.5f);
        F.body = io.Fonts->AddFontDefault(&cfg);
    }
    if (!F.strong) F.strong = F.body;
    if (!F.title) F.title = F.strong;
    if (!F.sm) F.sm = F.body;
    if (!F.caption) F.caption = F.sm;
    io.FontDefault = F.body;
    F.baked = s;
    return true;
}

void VarHudRenderer::ResetFonts() {
    F = FontSet{};
}

void VarHudRenderer::RequestReset() {
    g_resetPending.store(true, std::memory_order_release);
}

HudPage VarHudRenderer::CurrentPage() {
    return g_controller.Page();
}

bool VarHudRenderer::IsCapturingBinding() {
    return g_controller.IsCapturingBinding();
}

void VarHudRenderer::TickIdle() {
    auto& state = VarState::Instance();
    if (g_resetPending.exchange(false, std::memory_order_acq_rel)) {
        g_controller = VarHudController{};
    }
    if (g_controller.IsCapturingBinding() || InputManager::IsCapturing()) {
        g_controller.CancelBindingCapture(); // HUD hidden: never leave the input capture armed
        InputManager::CancelCapture();
    }
    state.SetInManualReplay(false);
    state.SetPanelFocused(false);
    if (state.IsMouseOverPanel()) {
        state.SetMouseOverPanel(false);
    }
    if (InputManager::ConsumePendingDebugToggle()) {
        state.SetDebugOverlayOpen(!state.IsDebugOverlayOpen());
    }
}

void VarHudRenderer::RenderFrame(float viewportW, float viewportH) {
    static HudRenderSnapshot s_renderSnap;
    auto& state = VarState::Instance();

    if (g_resetPending.exchange(false, std::memory_order_acq_rel)) {
        g_controller = VarHudController{};
        s_renderSnap = HudRenderSnapshot{};
        InputManager::CancelCapture();
    }

    ++g_hudFrame;
    // Single mutex lock + revision-cached players/teams/history/sync/markers (0 heap allocs on steady frames)
    state.CaptureHudRenderSnapshot(s_renderSnap);
    g_frameDataRevision = s_renderSnap.historyRev;
    g_frameSnapRevision = s_renderSnap.snapRev;
    g_frameNamesRevision = (static_cast<uint64_t>(s_renderSnap.rosterRev) << 32) ^ s_renderSnap.teamsRev;
    const LiveTelemetry& tel = s_renderSnap.tel;
    const VarVerdict& pending = s_renderSnap.pending;
    const VerdictPool& pool = s_renderSnap.pool;
    const HudConfig& cfg = s_renderSnap.cfg;
    g_lastScalePreset = cfg.scalePreset;
    S = (F.baked > 0.0f) ? F.baked : ComputeUiScale(viewportW, viewportH, cfg.scalePreset);

    g_controller.UpdateContext(tel, s_renderSnap.sync, s_renderSnap.players, s_renderSnap.teams,
                               s_renderSnap.history, s_renderSnap.stats, cfg,
                               s_renderSnap.rosterRev, s_renderSnap.teamsRev, s_renderSnap.historyRev);
    g_controller.UpdateReplayMarkers(s_renderSnap.markers);

    // Shortcut capture for the Controls page (runs before navigation so the captured input never navigates)
    if (g_controller.IsCapturingBinding()) {
        if (!g_controller.IsInManualReplay() || !g_controller.IsFocused() ||
            g_controller.Page() != HudPage::Controls || tel.siderOverlayOpen) {
            g_controller.CancelBindingCapture();
        } else {
            uint16_t code = 0;
            const InputManager::CaptureState cs = InputManager::PollCapture(code);
            if (cs == InputManager::CaptureState::Captured) {
                g_controller.ApplyCapturedBinding(code, state);
            } else if (cs != InputManager::CaptureState::Pending) {
                g_controller.CancelBindingCapture();
            }
        }
    } else if (InputManager::IsCapturing()) {
        InputManager::CancelCapture();
    }

    const auto actions = InputManager::PollActions(g_controller.IsInManualReplay(), g_controller.IsFocused(), tel.siderOverlayOpen);
    for (NavAction act : actions) {
        if (act == NavAction::Up || act == NavAction::Down || act == NavAction::Left || act == NavAction::Right ||
            act == NavAction::PrevTab || act == NavAction::NextTab || act == NavAction::QuickSwap) {
            g_scrollToSelection = true;
        }
        g_controller.HandleAction(act, state);
    }
    if (!actions.empty()) {
        state.CaptureHudRenderSnapshot(s_renderSnap);
    }
    g_pageAtFrameStart = g_controller.Page();

    state.SetInManualReplay(g_controller.IsInManualReplay());
    state.SetPanelFocused(g_controller.IsFocused());

    ApplyTheme();
    if (F.body) ImGui::PushFont(F.body);

    if (state.IsDebugOverlayOpen()) {
        RenderDebugTelemetry(viewportW, viewportH, tel, pending);
    }
    if (tel.lastBannerText[0] != '\0') {
        RenderBanner(viewportW, viewportH, tel.lastBannerText, tel.lastBannerStartMs, tel.lastBannerUntilMs, tel.lastBannerStyle, false);
    }
    if (cfg.broadcastAlerts && !g_controller.IsInManualReplay() && tel.alertBannerText[0] != '\0') {
        RenderBanner(viewportW, viewportH, tel.alertBannerText, tel.alertBannerStartMs, tel.alertBannerUntilMs, tel.alertBannerStyle, true);
    }

    // Update notice: shown in a VAR replay (never during play), once per game session for each state, so a
    // "new version" notice is followed by "installed" when the download finishes later
    {
        static int s_updateNoticeShown = 0; // phase already announced
        const UpdateChecker::Phase phase = UpdateChecker::CurrentPhase();
        const char* newer = UpdateChecker::NewerVersion();
        const bool announce = newer && (phase == UpdateChecker::Phase::Installed || phase == UpdateChecker::Phase::Updated ||
                                        (phase == UpdateChecker::Phase::Available && cfg.autoUpdate));
        if (announce && static_cast<int>(phase) > s_updateNoticeShown && g_controller.IsInManualReplay() &&
            !tel.siderOverlayOpen && GetTickCount64() >= tel.lastBannerUntilMs) {
            s_updateNoticeShown = static_cast<int>(phase);
            char text[128];
            if (phase == UpdateChecker::Phase::Updated) {
                std::snprintf(text, sizeof(text), "%s %s  \xC2\xB7  %s", Tr("VAR MOD ATUALIZADO PARA"), newer, UpdateChecker::kReleasesUrl);
                state.SetPostApplyBanner(text, 8000, BannerStyle::Applied);
            } else if (phase == UpdateChecker::Phase::Installed) {
                std::snprintf(text, sizeof(text), "%s %s  \xC2\xB7  %s", Tr("VAR MOD ATUALIZADO PARA"), newer, Tr("vale ao reiniciar o jogo"));
                state.SetPostApplyBanner(text, 8000, BannerStyle::Applied);
            } else {
                std::snprintf(text, sizeof(text), "%s %s  \xC2\xB7  %s", Tr("NOVA VERSÃO"), newer, UpdateChecker::kReleasesUrl);
                state.SetPostApplyBanner(text, 8000, BannerStyle::Info);
            }
        }
    }

    if (g_controller.IsInManualReplay() && !tel.siderOverlayOpen) {
        if (g_controller.IsFocused()) {
            RenderOpenPanel(viewportW, viewportH, state, pool);
            state.SetPanelFocused(g_controller.IsFocused());
        } else {
            state.SetMouseOverPanel(false);
            RenderCollapsedPill(viewportW, viewportH, pending, pool, cfg);
        }
    } else {
        state.SetMouseOverPanel(false);
    }

    RenderConfirmToast(viewportW, viewportH);

    if (F.body) ImGui::PopFont();
    // A navigation scroll request only applies to the frame it was made for (pages without the selected row
    // drawn would otherwise keep it and jump later)
    g_scrollToSelection = false;
}

} // namespace var_mod
