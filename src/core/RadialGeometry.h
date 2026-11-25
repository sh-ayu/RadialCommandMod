#pragma once

#include <utility>
#include <vector>

namespace radial::geom {

struct RectF {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;

    float width() const noexcept { return x1 - x0; }
    float height() const noexcept { return y1 - y0; }
    bool  contains(float x, float y) const noexcept { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
};

struct PointF {
    float x = 0.0f;
    float y = 0.0f;
};

struct SectorAngles {
    float startAngle = 0.0f; // Radians, 0 = right, -pi/2 = up (12 o'clock)
    float endAngle   = 0.0f;
    float midAngle   = 0.0f;
};

// Compute sector start/end/mid angles for `index` of `count` sectors (sector 0 centered at 12 o'clock).
// `gapRadians` trims each side by gapRadians * 0.5 for crisp visual separation.
SectorAngles computeSectorAngles(int index, int count, float gapRadians = 0.035f) noexcept;

// Compute label center point for a sector.
PointF sectorLabelCenter(float cx, float cy, float midRadius, int index, int count) noexcept;

// Rasterize an annular (donut) sector into non-overlapping horizontal scanline rects.
// Guarantees zero overlap so translucent colors render completely uniformly.
void rasterizeAnnularSector(
    float               cx,
    float               cy,
    float               innerR,
    float               outerR,
    float               startAngle,
    float               endAngle,
    float               rowStep,
    std::vector<RectF>& outRects
);

// Rasterize a filled circle (disc) into horizontal scanline rects.
void rasterizeDisc(
    float               cx,
    float               cy,
    float               radius,
    float               rowStep,
    std::vector<RectF>& outRects
);

// Rasterize a full circular ring (innerR..outerR) into horizontal scanline rects.
void rasterizeRing(
    float               cx,
    float               cy,
    float               innerR,
    float               outerR,
    float               rowStep,
    std::vector<RectF>& outRects
);

// Rasterize a line segment with half-thickness into axis-aligned runs (for pointer indicator).
void rasterizeLine(
    float               x0,
    float               y0,
    float               x1,
    float               y1,
    float               halfWidth,
    std::vector<RectF>& outRects
);

} // namespace radial::geom
