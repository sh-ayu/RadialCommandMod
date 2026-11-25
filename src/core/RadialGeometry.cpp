#include "core/RadialGeometry.h"

#include <algorithm>
#include <cmath>

namespace radial::geom {
namespace {

constexpr float kPi    = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;

float normalizeAngle(float a) noexcept {
    a = std::fmod(a, kTwoPi);
    if (a < 0.0f) a += kTwoPi;
    return a;
}

// Check if angle `a` lies inside the clockwise arc [start, start + sweep].
bool angleInArc(float a, float startNorm, float sweep) noexcept {
    if (sweep >= kTwoPi - 1e-4f) return true;
    float d = normalizeAngle(a - startNorm);
    return d <= sweep + 1e-5f;
}

// Given a horizontal line at relative Y `dy` and a sector ray at angle `theta`,
// return X where the ray intersects `y = dy` if the ray actually crosses that side of Y.
bool rayIntersectY(float dy, float theta, float& outX) noexcept {
    float s = std::sin(theta);
    float c = std::cos(theta);
    if (std::abs(s) < 1e-5f) return false;
    float t = dy / s;
    if (t < 0.0f) return false;
    outX = t * c;
    return true;
}

void emitClippedSpan(
    float               cx,
    float               y0,
    float               y1,
    float               dy,
    float               xL,
    float               xR,
    float               startNorm,
    float               sweep,
    std::vector<RectF>& out
) {
    if (!(xR > xL + 0.05f)) return;

    // Collect x-breakpoints where the two sector boundary rays cross this horizontal line.
    float pts[6];
    int   nPts = 0;
    pts[nPts++] = xL;
    pts[nPts++] = xR;

    float rx = 0.0f;
    if (rayIntersectY(dy, startNorm, rx) && rx > xL && rx < xR) {
        pts[nPts++] = rx;
    }
    if (rayIntersectY(dy, startNorm + sweep, rx) && rx > xL && rx < xR) {
        pts[nPts++] = rx;
    }
    // Also handle horizontal rays (sin ~= 0) at dy ~= 0 by splitting at x=0 if inside [xL, xR].
    if (xL < 0.0f && xR > 0.0f) {
        pts[nPts++] = 0.0f;
    }

    std::sort(pts, pts + nPts);
    for (int i = 0; i + 1 < nPts; ++i) {
        float a = pts[i];
        float b = pts[i + 1];
        if (b - a <= 0.05f) continue;
        float midX  = (a + b) * 0.5f;
        float angle = std::atan2(dy, midX);
        if (angleInArc(angle, startNorm, sweep)) {
            out.push_back(RectF{cx + a, y0, cx + b, y1});
        }
    }
}

} // namespace

SectorAngles computeSectorAngles(int index, int count, float gapRadians) noexcept {
    SectorAngles res{};
    if (count <= 0) return res;
    float span    = kTwoPi / static_cast<float>(count);
    float mid     = -kPi * 0.5f + span * static_cast<float>(index);
    float halfGap = std::min(gapRadians * 0.5f, span * 0.25f);
    res.midAngle   = mid;
    res.startAngle = mid - span * 0.5f + halfGap;
    res.endAngle   = mid + span * 0.5f - halfGap;
    return res;
}

PointF sectorLabelCenter(float cx, float cy, float midRadius, int index, int count) noexcept {
    auto a = computeSectorAngles(index, count, 0.0f);
    return PointF{
        cx + std::cos(a.midAngle) * midRadius,
        cy + std::sin(a.midAngle) * midRadius,
    };
}

void rasterizeAnnularSector(
    float               cx,
    float               cy,
    float               innerR,
    float               outerR,
    float               startAngle,
    float               endAngle,
    float               rowStep,
    std::vector<RectF>& outRects
) {
    if (!(outerR > 0.5f) || !(outerR > innerR)) return;
    float sweep = endAngle - startAngle;
    if (!(sweep > 1e-4f)) return;
    if (sweep > kTwoPi) sweep = kTwoPi;

    float step      = std::clamp(rowStep, 0.5f, 4.0f);
    float startNorm = normalizeAngle(startAngle);
    float rOut2     = outerR * outerR;
    float rIn2      = innerR > 0.0f ? (innerR * innerR) : 0.0f;

    int totalRows = std::max(1, static_cast<int>(std::ceil((2.0f * outerR) / step)));
    float yStart  = cy - outerR;
    for (int row = 0; row < totalRows; ++row) {
        float y0 = yStart + static_cast<float>(row) * step;
        float y1 = std::min(cy + outerR, y0 + step);
        float dy = ((y0 + y1) * 0.5f) - cy;
        float d2 = dy * dy;
        if (d2 >= rOut2) continue;

        float xOut = std::sqrt(std::max(0.0f, rOut2 - d2));
        if (rIn2 > 0.0f && d2 < rIn2) {
            float xIn = std::sqrt(std::max(0.0f, rIn2 - d2));
            emitClippedSpan(cx, y0, y1, dy, -xOut, -xIn, startNorm, sweep, outRects);
            emitClippedSpan(cx, y0, y1, dy, xIn, xOut, startNorm, sweep, outRects);
        } else {
            emitClippedSpan(cx, y0, y1, dy, -xOut, xOut, startNorm, sweep, outRects);
        }
    }
}

void rasterizeDisc(
    float               cx,
    float               cy,
    float               radius,
    float               rowStep,
    std::vector<RectF>& outRects
) {
    if (!(radius > 0.25f)) return;
    float step      = std::clamp(rowStep, 0.5f, 4.0f);
    float r2        = radius * radius;
    int   totalRows = std::max(1, static_cast<int>(std::ceil((2.0f * radius) / step)));
    float yStart    = cy - radius;
    for (int row = 0; row < totalRows; ++row) {
        float y0 = yStart + static_cast<float>(row) * step;
        float y1 = std::min(cy + radius, y0 + step);
        float dy = ((y0 + y1) * 0.5f) - cy;
        float d2 = dy * dy;
        if (d2 >= r2) continue;
        float xSpan = std::sqrt(std::max(0.0f, r2 - d2));
        if (xSpan > 0.1f) {
            outRects.push_back(RectF{cx - xSpan, y0, cx + xSpan, y1});
        }
    }
}

void rasterizeRing(
    float               cx,
    float               cy,
    float               innerR,
    float               outerR,
    float               rowStep,
    std::vector<RectF>& outRects
) {
    if (!(outerR > 0.5f) || !(outerR > innerR)) return;
    float step      = std::clamp(rowStep, 0.5f, 4.0f);
    float rOut2     = outerR * outerR;
    float rIn2      = innerR > 0.0f ? (innerR * innerR) : 0.0f;
    int   totalRows = std::max(1, static_cast<int>(std::ceil((2.0f * outerR) / step)));
    float yStart    = cy - outerR;
    for (int row = 0; row < totalRows; ++row) {
        float y0 = yStart + static_cast<float>(row) * step;
        float y1 = std::min(cy + outerR, y0 + step);
        float dy = ((y0 + y1) * 0.5f) - cy;
        float d2 = dy * dy;
        if (d2 >= rOut2) continue;
        float xOut = std::sqrt(std::max(0.0f, rOut2 - d2));
        if (rIn2 > 0.0f && d2 < rIn2) {
            float xIn = std::sqrt(std::max(0.0f, rIn2 - d2));
            if (xOut > xIn + 0.05f) {
                outRects.push_back(RectF{cx - xOut, y0, cx - xIn, y1});
                outRects.push_back(RectF{cx + xIn, y0, cx + xOut, y1});
            }
        } else if (xOut > 0.05f) {
            outRects.push_back(RectF{cx - xOut, y0, cx + xOut, y1});
        }
    }
}

void rasterizeLine(
    float               x0,
    float               y0,
    float               x1,
    float               y1,
    float               halfWidth,
    std::vector<RectF>& outRects
) {
    float dx    = x1 - x0;
    float dy    = y1 - y0;
    float major = std::max(std::abs(dx), std::abs(dy));
    if (!(major > 0.25f)) return;
    float hw    = std::max(0.5f, halfWidth);
    bool  horiz = std::abs(dx) >= std::abs(dy);
    float minor = horiz ? std::abs(dy) : std::abs(dx);
    int   runs  = (minor <= 0.1f) ? 1 : std::clamp(static_cast<int>(std::ceil(minor / std::max(0.75f, hw))), 1, 64);
    float drift = minor / static_cast<float>(runs);
    float h     = std::max(hw, drift * 0.55f);

    for (int i = 0; i < runs; ++i) {
        float t0 = static_cast<float>(i) / static_cast<float>(runs);
        float t1 = static_cast<float>(i + 1) / static_cast<float>(runs);
        float tm = (t0 + t1) * 0.5f;
        float cx = x0 + dx * tm;
        float cy = y0 + dy * tm;
        if (horiz) {
            float ax = x0 + dx * t0;
            float bx = x0 + dx * t1;
            outRects.push_back(RectF{std::min(ax, bx), cy - h, std::max(ax, bx), cy + h});
        } else {
            float ay = y0 + dy * t0;
            float by = y0 + dy * t1;
            outRects.push_back(RectF{cx - h, std::min(ay, by), cx + h, std::max(ay, by)});
        }
    }
}

} // namespace radial::geom
