// Local-shadow depth planes: reciprocal view depth is affine in perspective
// raster coordinates. Orthographic view depth itself is affine.
#ifndef CLEAN_PDN_PLANE_GLSL
#define CLEAN_PDN_PLANE_GLSL
float pdnPlaneSlope(float z, float positiveZ, float negativeZ, bool ortho)
{
    const float a = positiveZ > 0.0 ? (ortho ? positiveZ - z : 1.0 - z / positiveZ) : 1e30;
    const float b = negativeZ > 0.0 ? (ortho ? z - negativeZ : z / negativeZ - 1.0) : 1e30;
    const float slope = abs(a) <= abs(b) ? a : b;
    return abs(slope) < 1e29 ? clamp(slope, -60000.0, 60000.0) : 0.0;
}
float pdnPlaneDepth(float z, vec2 slope, vec2 offset, bool ortho)
{
    const float shift = dot(slope, offset);
    const float denominator = 1.0 - shift;
    const float predicted = ortho ? z + shift : (denominator > 1e-5 ? z / denominator : -1.0);
    return predicted > 0.0 && !isinf(predicted) ? predicted : -1.0;
}
vec2 pdnPlaneRaster(vec2 q, vec2 ratio, vec2 phase)
{
    return floor((q + 0.5 + phase) * ratio) + 0.5;
}
vec2 pdnPlaneRasterBetween(vec2 hc, vec2 ratio, vec2 phase)
{
    const vec2 b = floor(hc);
    return mix(pdnPlaneRaster(b, ratio, phase), pdnPlaneRaster(b + 1.0, ratio, phase), hc - b);
}
// Final denoiser guide alpha is FP16. Its rounding interval must not modulate
// four taps' weights every jitter phase. Keep the SAME rejection endpoint tol:
// only flatten the sub-ULP part, then rescale the remaining interval to e=1.
float pdnPlaneDepthMismatch(float storedZ, float expectedZ, float tolerance)
{
    if (!(storedZ > 0.0) || !(expectedZ > 0.0) || isinf(storedZ) || isinf(expectedZ)) return 1e30;
    const float uncertainty = min(max(storedZ, expectedZ) / 2048.0, 0.5 * tolerance);
    return max(abs(storedZ - expectedZ) - uncertainty, 0.0) / max(tolerance - uncertainty, 1e-6);
}
#endif
