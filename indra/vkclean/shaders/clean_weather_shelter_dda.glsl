// Shared scalar DDA for the avatar shelter heightfield. This subset is also included by the
// standalone C++ geometry fixture; keep the arithmetic and branch order in sync.
#ifndef CLEAN_WEATHER_SHELTER_DDA_GLSL
#define CLEAN_WEATHER_SHELTER_DDA_GLSL

#ifdef WX_SHELTER_DDA_CPU
#include <cmath>
static inline float shDdaFloor(float value) { return std::floor(value); }
static inline float shDdaMin(float a, float b) { return a < b ? a : b; }
static inline float shDdaMax(float a, float b) { return a > b ? a : b; }
static inline float shDdaSin(float value) { return std::sin(value); }
static inline float shDdaCos(float value) { return std::cos(value); }
static inline bool shDdaFinite(float value) { return std::isfinite(value); }
#else
float shDdaFloor(float value) { return floor(value); }
float shDdaMin(float a, float b) { return min(a, b); }
float shDdaMax(float a, float b) { return max(a, b); }
float shDdaSin(float value) { return sin(value); }
float shDdaCos(float value) { return cos(value); }
bool shDdaFinite(float value) { return !isnan(value) && !isinf(value); }
#endif

// Keep nonfinite atlas data visible to the DDA's fail-closed checks.
float shDdaNormalizeTop(float height, float frameZ, float noneTest)
{
    if (!shDdaFinite(height)) return height;
    return height > noneTest ? frameZ + height : -1.0e9;
}

// Return 0 for no crossing, 1 for a measured crossing, 2 for the fail-closed guard.
// WX_SH_DDA_TOP(layer, source, cellX, cellY, frameZ) supplies the atlas cell's world-space top.
int shShelterSegmentHit(float px, float py, float pz,
                        float shearX, float shearY,
                        float frameX, float frameY, float frameZ, float yaw, float extent,
                        int resolution, float headZ, float coverAbove,
                        int layer, int source, int requestedSteps)
{
    if (resolution <= 0 || resolution > 256 || !(extent > 0.0) || !shDdaFinite(extent) ||
        !shDdaFinite(px) || !shDdaFinite(py) || !shDdaFinite(pz) ||
        !shDdaFinite(shearX) || !shDdaFinite(shearY) ||
        !shDdaFinite(frameX) || !shDdaFinite(frameY) || !shDdaFinite(frameZ) ||
        !shDdaFinite(yaw) || !shDdaFinite(headZ) || !shDdaFinite(coverAbove)) return 2;

    const float c = shDdaCos(yaw), s = shDdaSin(yaw);
    const float dxWorld = -shearX, dyWorld = -shearY;
    const float x0 = ((c * (px - frameX) + s * (py - frameY)) / extent + 0.5) * float(resolution);
    const float y0 = ((-s * (px - frameX) + c * (py - frameY)) / extent + 0.5) * float(resolution);
    const float dx = (c * dxWorld + s * dyWorld) / extent * float(resolution);
    const float dy = (-s * dxWorld + c * dyWorld) / extent * float(resolution);
    if (!shDdaFinite(x0) || !shDdaFinite(y0) || !shDdaFinite(dx) || !shDdaFinite(dy)) return 2;

    const float infinity = 3.402823466e+38;
    float tEnter = 0.0, tExit = infinity;
    if (dx == 0.0)
    {
        if (x0 < 0.0 || x0 >= float(resolution)) return 0;
    }
    else
    {
        const float ta = (0.0 - x0) / dx;
        const float tb = (float(resolution) - x0) / dx;
        tEnter = shDdaMax(tEnter, shDdaMin(ta, tb));
        tExit = shDdaMin(tExit, shDdaMax(ta, tb));
    }
    if (dy == 0.0)
    {
        if (y0 < 0.0 || y0 >= float(resolution)) return 0;
    }
    else
    {
        const float ta = (0.0 - y0) / dy;
        const float tb = (float(resolution) - y0) / dy;
        tEnter = shDdaMax(tEnter, shDdaMin(ta, tb));
        tExit = shDdaMin(tExit, shDdaMax(ta, tb));
    }
    if (dx == 0.0 && dy == 0.0)
    {
        const int cellX = int(shDdaFloor(x0)), cellY = int(shDdaFloor(y0));
        const float top = WX_SH_DDA_TOP(layer, source, cellX, cellY, frameZ);
        if (!shDdaFinite(top)) return 2;
        return top > headZ + coverAbove && top >= pz - 0.02 ? 1 : 0;
    }
    if (!shDdaFinite(tEnter) || !shDdaFinite(tExit)) return 2;
    if (tExit < tEnter) return 0;
    if (tExit == tEnter) return 2;

    const float rawEnterX = x0 + dx * tEnter, rawEnterY = y0 + dy * tEnter;
    if (!shDdaFinite(rawEnterX) || !shDdaFinite(rawEnterY)) return 2;
    const float atEnterX = shDdaMax(0.0, shDdaMin(float(resolution), rawEnterX));
    const float atEnterY = shDdaMax(0.0, shDdaMin(float(resolution), rawEnterY));
    const float floorX = shDdaFloor(atEnterX), floorY = shDdaFloor(atEnterY);
    int cellX = int(floorX), cellY = int(floorY);
    // Entering through a grid edge while moving negative belongs to the cell on that side.
    if (dx < 0.0 && atEnterX == floorX) --cellX;
    if (dy < 0.0 && atEnterY == floorY) --cellY;
    // A positive slab interval must enter a valid cell; otherwise arithmetic
    // or boundary rounding made the traversal uncertain.
    if (cellX < 0 || cellX >= resolution || cellY < 0 || cellY >= resolution) return 2;

    const int stepX = dx > 0.0 ? 1 : (dx < 0.0 ? -1 : 0);
    const int stepY = dy > 0.0 ? 1 : (dy < 0.0 ? -1 : 0);
    float nextX = infinity, nextY = infinity;
    if (stepX > 0) nextX = (float(cellX + 1) - x0) / dx;
    else if (stepX < 0) nextX = (float(cellX) - x0) / dx;
    if (stepY > 0) nextY = (float(cellY + 1) - y0) / dy;
    else if (stepY < 0) nextY = (float(cellY) - y0) / dy;

    const int geometricLimit = 2 * resolution + 1;
    const int stepLimit = requestedSteps < geometricLimit ? requestedSteps : geometricLimit;
    if (stepLimit <= 0) return 2;
    float t = tEnter;
#ifdef WX_SHELTER_DDA_CPU
#define WX_SHELTER_DDA_LOOP_ATTRIBUTE
#else
#define WX_SHELTER_DDA_LOOP_ATTRIBUTE [[dont_unroll]]
#endif
    WX_SHELTER_DDA_LOOP_ATTRIBUTE
    for (int visit = 0; visit < stepLimit; ++visit)
    {
        // Ordinary exit returns at cellEnd >= tExit before advancing.
        if (cellX < 0 || cellX >= resolution || cellY < 0 || cellY >= resolution) return 2;
        const float cellEnd = shDdaMin(tExit, shDdaMin(nextX, nextY));
        if (!shDdaFinite(cellEnd) || cellEnd < t) return 2;
        const float top = WX_SH_DDA_TOP(layer, source, cellX, cellY, frameZ);
        if (!shDdaFinite(top)) return 2;
        if (top > headZ + coverAbove && top >= pz + t - 0.02 && top <= pz + cellEnd + 0.02) return 1;
        if (cellEnd >= tExit) return 0;

        bool advanceX = nextX <= cellEnd;
        bool advanceY = nextY <= cellEnd;
        if (!advanceX && !advanceY) return 2;
        if (advanceX)
        {
            cellX += stepX;
            nextX = stepX > 0 ? (float(cellX + 1) - x0) / dx : (float(cellX) - x0) / dx;
        }
        if (advanceY)
        {
            cellY += stepY;
            nextY = stepY > 0 ? (float(cellY + 1) - y0) / dy : (float(cellY) - y0) / dy;
        }
        if ((advanceX && nextX <= t) || (advanceY && nextY <= t)) return 2;
        t = cellEnd;
    }
    // The geometry-derived limit should be sufficient. If floating-point input defeats it,
    // prefer a conservative shelter result over allowing rain through an uncertain segment.
    return 2;
#undef WX_SHELTER_DDA_LOOP_ATTRIBUTE
}

#endif
