// vkclean — light transmittance through a translucent (alpha-blend) caster (#212).
// Shared by the shadow-map translucency pass (clean_shadow_trans.frag) and the
// ray-traced shadow rays (clean_rt_common.glsl), so both paths tint alike.
//   col = the caster's linear base colour (factor x tint x texel), a = its alpha.
// More alpha blocks more light: (1 - a) passes. What passes takes the caster's hue
// (colour normalised to its brightest channel, so dark glass doesn't double-darken),
// fading in over the first half of the alpha range: a faint film barely tints,
// half-opaque stained glass throws its full colour.
vec3 translucentTransmit(vec3 col, float a)
{
    a = clamp(a, 0.0, 1.0);
    const float m = max(max(col.r, col.g), col.b);
    const vec3 hue = m > 1e-4 ? col / m : vec3(1.0);
    return (1.0 - a) * mix(vec3(1.0), hue, smoothstep(0.0, 0.5, a));
}
