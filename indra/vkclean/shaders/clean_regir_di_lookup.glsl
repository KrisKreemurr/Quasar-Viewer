// Primary DI proposal lookup only. P, receiver geometry, target and rays do not move.
#ifndef CLEAN_REGIR_DI_LOOKUP_GLSL
#define CLEAN_REGIR_DI_LOOKUP_GLSL
uint rsPrimaryLookupCell(vec3 P, vec3 Ng, ivec2 renderPixel, uint frame)
{
    const uint base = regirCell(P);
    if (base == REGIR_NO_CELL || (rg.rs_cfg.z & RS_F_LOOKUP_JITTER) == 0u) return base;
    const float nn = dot(Ng, Ng);
    if (!(nn > 1e-12) || any(isnan(Ng)) || any(isinf(Ng))) return base;
    const vec3 n = Ng * inversesqrt(nn);
    const vec3 tangent = normalize(cross(abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0), n));
    const vec3 bitangent = cross(n, tangent);
    // The shadow sampler's existing interleaved-gradient noise and temporal R2
    // rotation, keyed by the ACTUAL FullP receiver selected by the D->P mapping.
    // No texture/descriptor or per-cell seed shared by neighbouring pixels.
    const vec2 px = vec2(renderPixel) + 0.5;
    const vec2 phase = fract(float(frame) * vec2(0.7548776662, 0.5698402910));
    const float u = fract(52.9829189 * fract(dot(px, vec2(0.06711056, 0.00583715))) + phase.x);
    const float v = fract(52.9829189 * fract(dot(px.yx + vec2(17.0, 59.0), vec2(0.06711056, 0.00583715))) + phase.y);
    const uint level = base / REGIR_CELLS_PER_LEVEL;
    const vec4 origin = rg.origin[level];
    // A half-cell-radius disk in the receiver tangent plane. The componentwise
    // world displacement is <=0.5*edge, matching the DI builder's support pad.
    const float radius = 0.5 * origin.w * sqrt(u);
    const float angle = 6.28318530718 * v;
    const vec3 lookup = P + radius * (cos(angle) * tangent + sin(angle) * bitangent);
    const vec3 cell = floor((lookup - origin.xyz) / origin.w);
    const uvec3 c = uvec3(clamp(cell, vec3(0.0), vec3(REGIR_DIM_X-1u, REGIR_DIM_Y-1u, REGIR_DIM_Z-1u)));
    // Stay at the original level: jitter never changes grid coverage or chooses
    // a finer level whose smaller support expansion could omit a source at P.
    return level * REGIR_CELLS_PER_LEVEL + (c.z * REGIR_DIM_Y + c.y) * REGIR_DIM_X + c.x;
}
#endif
