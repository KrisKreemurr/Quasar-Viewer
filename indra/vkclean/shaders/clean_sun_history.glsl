// Shared sun / moon history policy only; no sampling, storage or GI dependencies.
#ifndef CLEAN_SUN_HISTORY_GLSL
#define CLEAN_SUN_HISTORY_GLSL
// The separate mask encodes angular motion in 7 bits at 1/32 disc radius.
float sunHistoryMotion(float step)
{
    return floor(clamp(step, 0.0, 127.0 / 32.0) * 32.0 + 0.5) / 32.0;
}
float sunHistoryLimit(float r, bool dyn, bool hardSun, float full, float hardLength,
                      float dynamicMax, float staticFloor, bool selfIntegrated)
{
    full = max(full, 1.0);
    if (!dyn && selfIntegrated) return full;
    if (!dyn) return min(full, mix(staticFloor, full, smoothstep(1.0, 4.0, max(r, 0.0))));
    if (hardSun) return 1.0;
    // Match the mask's integer moving-caster history field, including fractional settings.
    const float hl = min(max(floor(hardLength + 0.5), 1.0), full);
    const float rr = max(r, 0.0);
    const float cap = min(full, rr < 1.0 ? mix(1.0, hl, rr) : mix(hl, full, smoothstep(1.0, 4.0, rr)));
    return dynamicMax > 0.0 ? min(cap, max(dynamicMax, hl)) : cap;
}
float sunHistoryMoveLimit(float r, float cap, float motion)
{
    const float mv = motion * max(r, 1.0);
    return mv > 0.0 ? clamp(0.5 / mv, 1.0, cap) : cap;
}
#endif
