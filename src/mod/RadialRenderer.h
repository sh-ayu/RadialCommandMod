#pragma once

#include "core/Localization.h"
#include "core/RadialConfig.h"
#include "core/RadialController.h"

#include <vector>

class MinecraftUIRenderContext;
namespace mce {
class Color;
}

namespace radial {

// Single-draw-call GPU triangle mesh batch for razor-smooth circles, annular sectors, rings, and lines.
// Replaces thousands of per-scanline fillRectangle() draw calls with ONE MeshHelpers::renderMeshImmediately() call!
class WheelMeshBatch {
public:
    struct Vertex {
        float x, y;
        float r, g, b, a;
    };

    void clear() noexcept { mVerts.clear(); }
    void reserve(size_t vertices) { mVerts.reserve(vertices); }
    bool empty() const noexcept { return mVerts.empty(); }

    void addAnnularSector(
        float             cx,
        float             cy,
        float             rIn,
        float             rOut,
        float             startAngle,
        float             endAngle,
        float             halfGapPx,
        mce::Color const& col,
        float             alpha
    );

    void addRing(
        float             cx,
        float             cy,
        float             rIn,
        float             rOut,
        mce::Color const& col,
        float             alpha
    );

    void addDisc(float cx, float cy, float radius, mce::Color const& col, float alpha);

    void addLine(
        float             x0,
        float             y0,
        float             x1,
        float             y1,
        float             halfWidth,
        mce::Color const& col,
        float             alpha
    );

    // Submits all accumulated triangles in ONE GPU draw call using Bedrock's ui_fill_gradient material.
    // Returns false if the GPU mesh path was unavailable (caller may fall back).
    bool flush(MinecraftUIRenderContext& ctx, float originX, float originY) const noexcept;

private:
    void pushTri(float x0, float y0, float x1, float y1, float x2, float y2, mce::Color const& col, float alpha);

    std::vector<Vertex> mVerts;
};

class RadialRenderer {
public:
    static void renderWheel(
        MinecraftUIRenderContext& ctx,
        float                     originX,
        float                     originY,
        float                     screenW,
        float                     screenH,
        RadialViewSnapshot const& snap,
        RadialConfig const&       cfg,
        ResolvedLang              lang
    ) noexcept;

    static void renderToast(
        MinecraftUIRenderContext& ctx,
        float                     originX,
        float                     originY,
        float                     screenW,
        ToastState const&         toast
    ) noexcept;
};

} // namespace radial
