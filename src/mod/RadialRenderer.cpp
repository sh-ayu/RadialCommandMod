#include "mod/RadialRenderer.h"

#include "core/RadialGeometry.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#include <mc/client/gui/CaretMeasureData.h>
#include <mc/client/gui/Font.h>
#include <mc/client/gui/FontHandle.h>
#include <mc/client/gui/ScreenRenderer.h>
#include <mc/client/gui/TextAlignment.h>
#include <mc/client/gui/TextMeasureData.h>
#include <mc/client/gui/screens/ScreenContext.h>
#include <mc/client/renderer/RenderMaterialGroup.h>
#include <mc/client/renderer/Tessellator.h>
#include <mc/client/renderer/screen/MinecraftUIRenderContext.h>
#include <mc/common/client/renderer/helpers/MeshHelpers.h>
#include <mc/deps/core/math/Color.h>
#include <mc/deps/core/string/HashedString.h>
#include <mc/deps/core_graphics/enums/PrimitiveMode.h>
#include <mc/deps/input/RectangleArea.h>
#include <mc/deps/minecraft_renderer/renderer/MaterialPtr.h>
#include <mc/deps/minecraft_renderer/resources/OffscreenCaptureDescription.h>
#include <mc/deps/renderer/ShaderColor.h>

namespace radial {
namespace {

inline mce::Color colorFromRgb(uint32_t rgb) noexcept {
    return mce::Color(
        static_cast<int>((rgb >> 16) & 0xFFu),
        static_cast<int>((rgb >> 8) & 0xFFu),
        static_cast<int>(rgb & 0xFFu)
    );
}

inline mce::Color blendColor(mce::Color const& a, mce::Color const& b, float t) noexcept {
    t = std::clamp(t, 0.0f, 1.0f);
    return mce::Color(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1.0f);
}

inline constexpr mce::Color kBgDark(14, 18, 26);
inline constexpr mce::Color kCenterBg(10, 14, 21);
inline constexpr mce::Color kBorderSoft(148, 163, 184);
inline constexpr mce::Color kWhite(248, 250, 252);
inline constexpr mce::Color kMuted(148, 163, 184);
inline constexpr mce::Color kAccentDefault(56, 189, 248);

mce::Color slotAccentColor(RadialSlot const& s) noexcept {
    if (auto rgb = parseHexColor(s.color)) return colorFromRgb(*rgb);
    switch (s.type) {
    case ActionType::Command:       return mce::Color(56, 189, 248);
    case ActionType::Chat:          return mce::Color(52, 211, 153);
    case ActionType::ClientCommand: return mce::Color(245, 158, 11);
    case ActionType::Macro:         return mce::Color(167, 139, 250);
    case ActionType::Submenu:       return mce::Color(96, 165, 250);
    }
    return kAccentDefault;
}

char const* slotMiniBadge(ActionType t) noexcept {
    switch (t) {
    case ActionType::Command:       return "/";
    case ActionType::Chat:          return "C";
    case ActionType::ClientCommand: return "*";
    case ActionType::Macro:         return "M";
    case ActionType::Submenu:       return ">";
    }
    return "";
}

RectangleArea toArea(geom::RectF const& r, float ox, float oy) noexcept {
    RectangleArea a;
    a._x0 = r.x0 + ox;
    a._y0 = r.y0 + oy;
    a._x1 = r.x1 + ox;
    a._y1 = r.y1 + oy;
    return a;
}

Font* getLiveFont(MinecraftUIRenderContext& ctx) noexcept {
    try {
        return ctx.mDebugTextFontHandle.get().mDefaultFont.get();
    } catch (...) {
        return nullptr;
    }
}

TextWidthMeasureFn makeMeasureFn(Font* font, float fontScale) noexcept {
    return [font, fontScale](std::string_view sv) -> float {
        if (sv.empty()) return 0.0f;
        if (font) {
            try {
                int px = font->getLineLength(sv, fontScale, false);
                if (px > 0) return static_cast<float>(px);
            } catch (...) {
            }
        }
        return estimateDisplayWidth(sv, fontScale);
    };
}

void drawText(
    MinecraftUIRenderContext& ctx,
    Font*                     font,
    geom::RectF const&        r,
    float                     ox,
    float                     oy,
    std::string               text,
    mce::Color const&         color,
    float                     alpha,
    float                     scale,
    ::ui::TextAlignment       align  = ::ui::TextAlignment::Center,
    bool                      shadow = true
) noexcept {
    if (alpha <= 0.01f || text.empty()) return;
    try {
        TextMeasureData  measure{scale, 0.0f, shadow, false, false, align};
        CaretMeasureData caret{0, false};
        auto             area = toArea(r, ox, oy);
        if (font) ctx.drawText(*font, area, std::move(text), color, alpha, align, measure, caret);
        else ctx.drawDebugText(area, std::move(text), color, alpha, align, measure, caret);
    } catch (...) {
    }
}

void fillRects(
    MinecraftUIRenderContext&       ctx,
    std::vector<geom::RectF> const& rects,
    float                           ox,
    float                           oy,
    mce::Color const&               color,
    float                           alpha
) noexcept {
    if (alpha <= 0.005f) return;
    for (auto const& r : rects) {
        ctx.fillRectangle(toArea(r, ox, oy), color, alpha);
    }
}

std::string slotPreviewLine(RadialSlot const& s, ResolvedLang lang) {
    if (s.type == ActionType::Submenu) {
        return std::string(tr(lang, "hud.submenu_tag")) + " (" + std::to_string(s.children.size()) + ")";
    }
    if (s.type == ActionType::Macro) {
        return std::string(tr(lang, "hud.macro_tag")) + " (" + std::to_string(s.steps.size()) + " " + tr(lang, "hud.steps") + ")";
    }
    if (!s.value.empty()) return s.value;
    return std::string(actionTypeToString(s.type));
}

// Per-sector smooth hover interpolation state (UI thread only).
struct HoverAnimState {
    std::array<float, kMaxSlotsPerMenu> weight{};
    std::chrono::steady_clock::time_point lastTick{};
    bool                                  wasOpen = false;
};

HoverAnimState& animState() noexcept {
    static HoverAnimState st;
    return st;
}

} // namespace

void WheelMeshBatch::pushTri(
    float             x0,
    float             y0,
    float             x1,
    float             y1,
    float             x2,
    float             y2,
    mce::Color const& col,
    float             alpha
) {
    mVerts.push_back(Vertex{x0, y0, col.r, col.g, col.b, alpha});
    mVerts.push_back(Vertex{x1, y1, col.r, col.g, col.b, alpha});
    mVerts.push_back(Vertex{x2, y2, col.r, col.g, col.b, alpha});
}

void WheelMeshBatch::addAnnularSector(
    float             cx,
    float             cy,
    float             rIn,
    float             rOut,
    float             startAngle,
    float             endAngle,
    float             halfGapPx,
    mce::Color const& col,
    float             alpha
) {
    if (alpha <= 0.004f || !(rOut > rIn + 0.05f)) return;
    float const sweep = endAngle - startAngle;
    if (!(sweep > 1e-4f)) return;

    float const dIn   = (halfGapPx > 0.0f && rIn > 1.0f) ? std::min(halfGapPx / rIn, sweep * 0.25f) : 0.0f;
    float const dOut  = (halfGapPx > 0.0f && rOut > 1.0f) ? std::min(halfGapPx / rOut, sweep * 0.25f) : 0.0f;
    float const aIn0  = startAngle + dIn;
    float const aIn1  = endAngle - dIn;
    float const aOut0 = startAngle + dOut;
    float const aOut1 = endAngle - dOut;
    if (!(aIn1 > aIn0 + 1e-4f) || !(aOut1 > aOut0 + 1e-4f)) return;

    int const   steps = std::clamp(static_cast<int>(std::ceil(sweep * rOut / 3.2f)), 8, 56);
    float const sIn   = (aIn1 - aIn0) / static_cast<float>(steps);
    float const sOut  = (aOut1 - aOut0) / static_cast<float>(steps);

    float prevInX  = cx + std::cos(aIn0) * rIn;
    float prevInY  = cy + std::sin(aIn0) * rIn;
    float prevOutX = cx + std::cos(aOut0) * rOut;
    float prevOutY = cy + std::sin(aOut0) * rOut;

    for (int s = 1; s <= steps; ++s) {
        float const tIn  = aIn0 + sIn * static_cast<float>(s);
        float const tOut = aOut0 + sOut * static_cast<float>(s);
        float const inX  = cx + std::cos(tIn) * rIn;
        float const inY  = cy + std::sin(tIn) * rIn;
        float const outX = cx + std::cos(tOut) * rOut;
        float const outY = cy + std::sin(tOut) * rOut;

        // Winding verified cross_screen < 0 (exact CCW in Y-flipped UI ortho projection).
        pushTri(prevInX, prevInY, inX, inY, outX, outY, col, alpha);
        pushTri(prevInX, prevInY, outX, outY, prevOutX, prevOutY, col, alpha);

        prevInX  = inX;
        prevInY  = inY;
        prevOutX = outX;
        prevOutY = outY;
    }
}

void WheelMeshBatch::addRing(
    float             cx,
    float             cy,
    float             rIn,
    float             rOut,
    mce::Color const& col,
    float             alpha
) {
    if (alpha <= 0.004f || !(rOut > rIn + 0.05f)) return;
    constexpr float kTwoPi = 6.28318530718f;
    int const       steps  = std::clamp(static_cast<int>(std::ceil(kTwoPi * rOut / 3.5f)), 24, 72);
    float const     da     = kTwoPi / static_cast<float>(steps);

    float prevInX  = cx + rIn;
    float prevInY  = cy;
    float prevOutX = cx + rOut;
    float prevOutY = cy;

    for (int s = 1; s <= steps; ++s) {
        float const a    = (s == steps) ? 0.0f : (da * static_cast<float>(s));
        float const ca   = std::cos(a);
        float const sa   = std::sin(a);
        float const inX  = cx + ca * rIn;
        float const inY  = cy + sa * rIn;
        float const outX = cx + ca * rOut;
        float const outY = cy + sa * rOut;

        pushTri(prevInX, prevInY, inX, inY, outX, outY, col, alpha);
        pushTri(prevInX, prevInY, outX, outY, prevOutX, prevOutY, col, alpha);

        prevInX  = inX;
        prevInY  = inY;
        prevOutX = outX;
        prevOutY = outY;
    }
}

void WheelMeshBatch::addDisc(float cx, float cy, float radius, mce::Color const& col, float alpha) {
    if (alpha <= 0.004f || !(radius > 0.25f)) return;
    constexpr float kTwoPi = 6.28318530718f;
    int const       steps  = std::clamp(static_cast<int>(std::ceil(kTwoPi * radius / 3.0f)), 16, 64);
    float const     da     = kTwoPi / static_cast<float>(steps);

    float prevX = cx + radius;
    float prevY = cy;
    for (int s = 1; s <= steps; ++s) {
        float const a  = (s == steps) ? 0.0f : (da * static_cast<float>(s));
        float const nx = cx + std::cos(a) * radius;
        float const ny = cy + std::sin(a) * radius;
        pushTri(cx, cy, nx, ny, prevX, prevY, col, alpha);
        prevX = nx;
        prevY = ny;
    }
}

void WheelMeshBatch::addLine(
    float             x0,
    float             y0,
    float             x1,
    float             y1,
    float             halfWidth,
    mce::Color const& col,
    float             alpha
) {
    if (alpha <= 0.004f) return;
    float const dx  = x1 - x0;
    float const dy  = y1 - y0;
    float const len = std::sqrt(dx * dx + dy * dy);
    if (!(len > 0.25f)) return;
    float const hw = std::max(0.4f, halfWidth);
    float const nx = (-dy / len) * hw;
    float const ny = (dx / len) * hw;

    float const ax = x0 + nx, ay = y0 + ny;
    float const bx = x0 - nx, by = y0 - ny;
    float const cx = x1 - nx, cy = y1 - ny;
    float const dx2 = x1 + nx, dy2 = y1 + ny;

    pushTri(ax, ay, cx, cy, bx, by, col, alpha);
    pushTri(ax, ay, dx2, dy2, cx, cy, col, alpha);
}

bool WheelMeshBatch::flush(MinecraftUIRenderContext& ctx, float originX, float originY) const noexcept {
    if (mVerts.empty()) return true;
    try {
        ScreenContext& screenCtx = ctx.mScreenContext;
        Tessellator&   tess      = screenCtx.tessellator;
        if (static_cast<bool>(tess.mTessellating)) return false;

        auto&       mats = ScreenRenderer::mScreenMaterials();
        auto const& mat  = mats[static_cast<int>(ScreenRenderer::ScreenMaterial::FillGradient)];
        if (!static_cast<bool>(mat.mRenderMaterialInfoPtr.get())) return false;

        struct ShaderColorGuard {
            ShaderColor& target;
            mce::Color   saved;
            explicit ShaderColorGuard(ShaderColor& sc) noexcept : target(sc), saved(sc.color) {
                target.color = mce::Color(1.0f, 1.0f, 1.0f, 1.0f);
                target.dirty = true;
            }
            ~ShaderColorGuard() noexcept {
                target.color = saved;
                target.dirty = true;
            }
            ShaderColorGuard(ShaderColorGuard const&)            = delete;
            ShaderColorGuard& operator=(ShaderColorGuard const&) = delete;
        };
        ShaderColorGuard shaderGuard(screenCtx.currentShaderColor);

        tess.begin(
            Tessellator::DebugContextCallback{},
            mce::PrimitiveMode::TriangleList,
            static_cast<int>(mVerts.size()),
            false
        );
        for (auto const& v : mVerts) {
            tess.color(v.r, v.g, v.b, v.a);
            tess.vertex(v.x + originX, v.y + originY, 0.0f);
        }

        OffscreenCaptureDescription capture{};
        MeshHelpers::renderMeshImmediately(screenCtx, tess, mat, capture);
        return true;
    } catch (...) {
        return false;
    }
}

void RadialRenderer::renderWheel(
    MinecraftUIRenderContext& ctx,
    float                     originX,
    float                     originY,
    float                     screenW,
    float                     screenH,
    RadialViewSnapshot const& snap,
    RadialConfig const&       cfg,
    ResolvedLang              lang
) noexcept {
    try {
        auto& animSt = animState();
        if (!snap.open || snap.visibleSlots.empty()) {
            animSt.wasOpen = false;
            animSt.weight.fill(0.0f);
            return;
        }

        // Compute smooth frame dt for per-sector hover easing.
        auto  now = std::chrono::steady_clock::now();
        float dt  = 0.016f;
        if (animSt.wasOpen) {
            dt = std::clamp(std::chrono::duration<float>(now - animSt.lastTick).count(), 0.001f, 0.08f);
        } else {
            animSt.weight.fill(0.0f);
        }
        animSt.wasOpen  = true;
        animSt.lastTick = now;

        int const count = std::min<int>(static_cast<int>(snap.visibleSlots.size()), kMaxSlotsPerMenu);
        float     lerpK = 1.0f - std::exp(-22.0f * dt);
        for (int i = 0; i < count; ++i) {
            float target = (i == snap.hoveredIndex) ? 1.0f : 0.0f;
            animSt.weight[static_cast<size_t>(i)] += (target - animSt.weight[static_cast<size_t>(i)]) * lerpK;
        }

        Font* const font      = getLiveFont(ctx);
        float const anim      = std::clamp(snap.openProgress, 0.05f, 1.0f);
        float const scaleAnim = 0.85f + 0.15f * anim;
        float const alpha     = anim;

        // Safe screen-height clamp so wheel + header pill + footer bar never clip off-screen!
        float const maxSafeOuter = std::max(44.0f, std::min(screenW * 0.42f, screenH * 0.5f - 40.0f));
        float const baseOuter    = std::min(cfg.outerRadius, maxSafeOuter);
        float const radiusRatio  = baseOuter / std::max(1.0f, cfg.outerRadius);
        float const outerR       = baseOuter * scaleAnim;
        float const innerR       = std::clamp(cfg.innerRadius * radiusRatio * scaleAnim, 14.0f, outerR - 18.0f);
        float const deadzoneR    = std::clamp(cfg.deadzoneRadius * radiusRatio * scaleAnim, 8.0f, outerR - 10.0f);
        float const popOutMax    = 5.5f * scaleAnim;
        float const cx           = screenW * 0.5f;
        float const cy           = screenH * 0.5f;

        // Center hub sits flush against innerR so there is ZERO awkward gap between hub & sectors!
        float const hubR = innerR;
        mce::Color  hubRingCol = kBorderSoft;
        if (snap.hoveredIndex >= 0 && snap.hoveredIndex < count) {
            hubRingCol = slotAccentColor(snap.visibleSlots[static_cast<size_t>(snap.hoveredIndex)]);
        }

        // Build ENTIRE wheel geometry into ONE GPU triangle batch -> 1 single draw call (~100x faster than fillRectangle scanlines!).
        thread_local WheelMeshBatch mesh;
        mesh.clear();
        mesh.reserve(2400);

        // 1) Soft backdrop disc behind the entire wheel (fills hairline sector dividers cleanly!).
        mesh.addDisc(cx, cy, outerR + 2.0f, mce::Color(6, 9, 15), 0.44f * alpha);

        // 2) Seamless annular sectors with uniform 1.2px parallel dividers and flush outer accent rim.
        float const halfGapPx = 0.65f;
        for (int i = 0; i < count; ++i) {
            auto const& slot   = snap.visibleSlots[static_cast<size_t>(i)];
            float const hw     = animSt.weight[static_cast<size_t>(i)];
            auto const  angles = geom::computeSectorAngles(i, count, 0.0f);
            mce::Color  accent = slotAccentColor(slot);

            float const rIn      = innerR;
            float const rOut     = outerR + popOutMax * hw;
            float const rimThick = 2.0f + 1.6f * hw;
            float const rRimIn   = std::max(rIn + 2.0f, rOut - rimThick);

            mce::Color fillCol = blendColor(kBgDark, accent, 0.34f * hw);
            float      fillA   = std::min(0.95f, cfg.backgroundOpacity + 0.14f * hw) * alpha;

            // Body & accent rim share exact rRimIn boundary (zero gap, zero double-alpha seam!).
            mesh.addAnnularSector(cx, cy, rIn, rRimIn, angles.startAngle, angles.endAngle, halfGapPx, fillCol, fillA);
            mesh.addAnnularSector(cx, cy, rRimIn, rOut, angles.startAngle, angles.endAngle, halfGapPx, accent, (0.55f + 0.43f * hw) * alpha);

            // Submenu auto-enter progress arc along inner rim.
            if (i == snap.hoveredIndex && slot.type == ActionType::Submenu && snap.submenuHoverRatio > 0.02f) {
                float sweep = (angles.endAngle - angles.startAngle) * snap.submenuHoverRatio;
                mesh.addAnnularSector(cx, cy, rIn, rIn + 2.8f, angles.startAngle, angles.startAngle + sweep, halfGapPx, kWhite, 0.94f * alpha);
            }
        }

        // 3) Center hub disc + crisp hub border ring flush with innerR.
        mesh.addDisc(cx, cy, hubR, kCenterBg, std::min(0.94f, cfg.backgroundOpacity + 0.12f) * alpha);
        mesh.addRing(cx, cy, std::max(1.0f, hubR - 1.4f), hubR + 0.2f, hubRingCol, (snap.hoveredIndex >= 0 ? 0.92f : 0.48f) * alpha);

        // Show subtle deadzone ring only when deadzone differs noticeably from hubR.
        if (std::abs(deadzoneR - hubR) > 6.0f) {
            mesh.addRing(cx, cy, std::max(1.0f, deadzoneR - 0.7f), deadzoneR, kMuted, 0.24f * alpha);
        }

        // 4) Clean directional pointer emerging from hub edge.
        float ptrX    = snap.pointerX * radiusRatio * scaleAnim;
        float ptrY    = snap.pointerY * radiusRatio * scaleAnim;
        float ptrDist = std::sqrt(ptrX * ptrX + ptrY * ptrY);
        if (ptrDist > 2.5f) {
            float maxVis = outerR + popOutMax - 3.0f;
            if (ptrDist > maxVis) {
                float k = maxVis / ptrDist;
                ptrX *= k;
                ptrY *= k;
                ptrDist = maxVis;
            }
            mce::Color ptrCol = (snap.hoveredIndex >= 0) ? hubRingCol : kMuted;
            float      ux     = ptrX / ptrDist;
            float      uy     = ptrY / ptrDist;
            if (ptrDist > hubR + 2.0f) {
                mesh.addLine(cx + ux * hubR, cy + uy * hubR, cx + ptrX, cy + ptrY, 1.0f, ptrCol, 0.78f * alpha);
            }
            mesh.addDisc(cx + ptrX, cy + ptrY, 2.8f, kWhite, 0.96f * alpha);
        }

        // Submit entire wheel in 1 GPU draw call; fall back cleanly to coarse scanlines if unavailable.
        if (!mesh.flush(ctx, originX, originY)) {
            std::vector<geom::RectF> buf;
            buf.reserve(256);
            for (int i = 0; i < count; ++i) {
                auto const& slot   = snap.visibleSlots[static_cast<size_t>(i)];
                float const hw     = animSt.weight[static_cast<size_t>(i)];
                auto const  angles = geom::computeSectorAngles(i, count, 0.015f);
                mce::Color  accent = slotAccentColor(slot);
                buf.clear();
                geom::rasterizeAnnularSector(cx, cy, innerR, outerR + popOutMax * hw, angles.startAngle, angles.endAngle, 2.0f, buf);
                fillRects(ctx, buf, originX, originY, blendColor(kBgDark, accent, 0.34f * hw), cfg.backgroundOpacity * alpha);
            }
            buf.clear();
            geom::rasterizeDisc(cx, cy, hubR, 2.0f, buf);
            fillRects(ctx, buf, originX, originY, kCenterBg, 0.92f * alpha);
        }

        // 5) Sector labels fitted strictly inside arc-length budget (no collisions even at 12 slots!).
        float const labelRadius = innerR + (outerR - innerR) * 0.54f;
        float const labelScale  = cfg.fontScale * (count > 8 ? 0.78f : 0.92f);
        float const subScale    = labelScale * 0.72f;
        auto const  measureMain = makeMeasureFn(font, labelScale);
        auto const  measureSub  = makeMeasureFn(font, subScale);

        constexpr float kTwoPi  = 6.28318530718f;
        float const     arcSpan = (kTwoPi * labelRadius / static_cast<float>(count)) * 0.86f;
        float const     radialW = (outerR - innerR) * 1.45f;
        float const     boxW    = std::clamp(std::min(arcSpan, radialW), 28.0f, 92.0f);

        for (int i = 0; i < count; ++i) {
            auto const& slot = snap.visibleSlots[static_cast<size_t>(i)];
            float const hw   = animSt.weight[static_cast<size_t>(i)];
            float const rMid = labelRadius + popOutMax * 0.45f * hw;
            auto const  pt   = geom::sectorLabelCenter(cx, cy, rMid, i, count);

            std::string rawLbl = slot.label;
            if (slot.type == ActionType::Submenu) rawLbl += " >";
            std::string mainLbl = fitTextToWidth(rawLbl, boxW - 2.0f, labelScale, measureMain);

            std::string subRaw = slot.subtitle;
            if (subRaw.empty()) {
                if (slot.type == ActionType::Macro) subRaw = tr(lang, "hud.macro_tag");
                else if (slot.type == ActionType::Submenu) subRaw = tr(lang, "hud.submenu_tag");
                else if (slot.type == ActionType::Command && !slot.value.empty()) subRaw = slot.value;
                else subRaw = slotMiniBadge(slot.type);
            }
            std::string subLbl = fitTextToWidth(subRaw, boxW - 2.0f, subScale, measureSub);

            float lineH  = 9.0f * labelScale;
            float subH   = 8.5f * subScale;
            mce::Color tc = (hw > 0.35f) ? kWhite : mce::Color(226, 232, 240);

            if (!subLbl.empty() && count <= 10 && (outerR - innerR) >= 34.0f) {
                float totalH = lineH + subH + 1.0f;
                float topY   = pt.y - totalH * 0.5f;
                drawText(ctx, font, {pt.x - boxW * 0.5f, topY, pt.x + boxW * 0.5f, topY + lineH}, originX, originY, mainLbl, tc, alpha, labelScale);
                drawText(ctx, font, {pt.x - boxW * 0.5f, topY + lineH + 1.0f, pt.x + boxW * 0.5f, topY + totalH}, originX, originY, subLbl, hw > 0.35f ? slotAccentColor(slot) : kMuted, 0.90f * alpha, subScale);
            } else {
                drawText(ctx, font, {pt.x - boxW * 0.5f, pt.y - lineH * 0.5f, pt.x + boxW * 0.5f, pt.y + lineH * 0.5f}, originX, originY, mainLbl, tc, alpha, labelScale);
            }
        }

        // 6) Center hub text strictly fitted to inner diameter.
        float const hubTextW = std::max(20.0f, (hubR - 4.0f) * 2.0f);
        if (snap.hoveredIndex >= 0 && snap.hoveredIndex < count) {
            auto const& sel     = snap.visibleSlots[static_cast<size_t>(snap.hoveredIndex)];
            float const s1      = 0.78f;
            float const s2      = 0.64f;
            std::string line1   = fitTextToWidth(sel.label, hubTextW, s1, makeMeasureFn(font, s1));
            std::string line2   = fitTextToWidth(slotPreviewLine(sel, lang), hubTextW, s2, makeMeasureFn(font, s2));
            drawText(ctx, font, {cx - hubTextW * 0.5f, cy - 8.5f, cx + hubTextW * 0.5f, cy}, originX, originY, line1, kWhite, alpha, s1);
            drawText(ctx, font, {cx - hubTextW * 0.5f, cy + 1.0f, cx + hubTextW * 0.5f, cy + 9.5f}, originX, originY, line2, hubRingCol, 0.92f * alpha, s2);
        } else {
            float const sc        = 0.70f;
            std::string centerTxt = fitTextToWidth(tr(lang, snap.inSubmenu ? "hud.back" : "hud.cancel"), hubTextW, sc, makeMeasureFn(font, sc));
            drawText(ctx, font, {cx - hubTextW * 0.5f, cy - 4.5f, cx + hubTextW * 0.5f, cy + 5.5f}, originX, originY, centerTxt, kMuted, 0.88f * alpha, sc);
        }

        // 7) Top header pill measured accurately with Font::getLineLength (works perfectly for Japanese & English!).
        {
            std::string header = snap.breadcrumb;
            if (snap.presetCount > 1) {
                header += " [" + std::to_string(snap.activePresetIndex + 1) + "/" + std::to_string(snap.presetCount) + "]";
            }
            if (snap.manualOverride) header += " *";

            float const sHdr    = 0.80f;
            float const sSrv    = 0.66f;
            auto const  mHdr    = makeMeasureFn(font, sHdr);
            auto const  mSrv    = makeMeasureFn(font, sSrv);
            float const maxPilW = std::max(100.0f, screenW - 20.0f);

            std::string fitHdr = fitTextToWidth(header, maxPilW - 14.0f, sHdr, mHdr);
            std::string fitSrv = fitTextToWidth(snap.serverDisplay, maxPilW - 14.0f, sSrv, mSrv);
            float       needW  = std::max(mHdr(fitHdr), mSrv(fitSrv)) + 16.0f;
            float       pillW  = std::clamp(needW, 96.0f, maxPilW);
            float       pillH  = 21.0f;
            float       pillY  = std::max(4.0f, cy - outerR - popOutMax - pillH - 6.0f);
            geom::RectF pill{cx - pillW * 0.5f, pillY, cx + pillW * 0.5f, pillY + pillH};

            ctx.fillRectangle(toArea(pill, originX, originY), kCenterBg, 0.84f * alpha);
            ctx.drawRectangle(toArea(pill, originX, originY), hubRingCol, 0.58f * alpha, 1);
            drawText(ctx, font, {pill.x0 + 4.0f, pill.y0 + 2.0f, pill.x1 - 4.0f, pill.y0 + 11.5f}, originX, originY, fitHdr, kWhite, alpha, sHdr);
            drawText(ctx, font, {pill.x0 + 4.0f, pill.y0 + 11.5f, pill.x1 - 4.0f, pill.y1 - 1.0f}, originX, originY, fitSrv, kMuted, 0.90f * alpha, sSrv);
        }

        // 8) Bottom hint bar measured accurately & fitted strictly inside screenW.
        {
            std::string footer;
            if (snap.hoveredIndex >= 0 && snap.hoveredIndex < count) {
                auto const& sel = snap.visibleSlots[static_cast<size_t>(snap.hoveredIndex)];
                footer          = sel.label + " : " + slotPreviewLine(sel, lang);
            } else {
                bool isToggle = (cfg.activationMode == ActivationMode::Toggle);
                char const* key = isToggle ? (snap.inSubmenu ? "hud.hint.toggle_sub" : "hud.hint.toggle_root")
                                           : (snap.inSubmenu ? "hud.hint.flick_sub"  : "hud.hint.flick_root");
                footer = tr(lang, key);
                if (cfg.showNumberHints && snap.presetCount > 1) {
                    footer += tr(lang, "hud.hint.preset");
                }
            }
            float const sFoot = 0.70f;
            auto const  mFoot = makeMeasureFn(font, sFoot);
            float const maxFW = std::max(100.0f, screenW - 18.0f);
            std::string fitF  = fitTextToWidth(footer, maxFW - 12.0f, sFoot, mFoot);
            float       fw    = std::clamp(mFoot(fitF) + 14.0f, 100.0f, maxFW);
            float       fh    = 13.0f;
            float       fy    = std::min(screenH - fh - 4.0f, cy + outerR + popOutMax + 6.0f);
            geom::RectF bar{cx - fw * 0.5f, fy, cx + fw * 0.5f, fy + fh};
            ctx.fillRectangle(toArea(bar, originX, originY), kCenterBg, 0.80f * alpha);
            drawText(ctx, font, {bar.x0 + 4.0f, bar.y0 + 2.0f, bar.x1 - 4.0f, bar.y1}, originX, originY, fitF, kWhite, 0.94f * alpha, sFoot);
        }

        ctx.flushText(0.016f, std::nullopt);
    } catch (...) {
    }
}

void RadialRenderer::renderToast(
    MinecraftUIRenderContext& ctx,
    float                     originX,
    float                     originY,
    float                     screenW,
    ToastState const&         toast
) noexcept {
    try {
        if (toast.alpha <= 0.01f || toast.text.empty()) return;
        Font* const font  = getLiveFont(ctx);
        float const sc    = 0.76f;
        auto const  mFn   = makeMeasureFn(font, sc);
        float const maxW  = std::max(90.0f, screenW - 24.0f);
        std::string fitT  = fitTextToWidth(toast.text, maxW - 14.0f, sc, mFn);
        float       w     = std::clamp(mFn(fitT) + 16.0f, 84.0f, maxW);
        float       h     = 14.0f;
        float       x0    = (screenW - w) * 0.5f;
        float       y0    = 10.0f;
        geom::RectF box{x0, y0, x0 + w, y0 + h};
        ctx.fillRectangle(toArea(box, originX, originY), kCenterBg, 0.82f * toast.alpha);
        ctx.drawRectangle(toArea(box, originX, originY), kAccentDefault, 0.68f * toast.alpha, 1);
        drawText(ctx, font, {x0 + 4.0f, y0 + 2.5f, x0 + w - 4.0f, y0 + h}, originX, originY, fitT, kWhite, toast.alpha, sc);
        ctx.flushText(0.016f, std::nullopt);
    } catch (...) {
    }
}

} // namespace radial
