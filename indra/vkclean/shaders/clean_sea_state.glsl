// vkclean -- THE SEA STATE (#1020; C++ indra/newview/slseastate.h has the model and the inputs).
//
// One shared description of the water surface: the waves' displacement and normal, and the
// masks the foam / splash look reads (breaking, crest, swash). A pure function of the world
// position (agent xy) and this frame's constants, so the water VS, the water FS, the RT forward
// reflections and any later reader evaluate the SAME surface. Include after clean_frame_info.glsl.
//
// FRAME INFO (ssr.sea, LLCleanRendererVK from SLSeaState::fillFrameInfo; zeros = flat water,
// except sea[0].yzw, sea[1].xyz and sea[2].z, which flat water keeps -- fillSeaInfo):
//   sea[0]  x mode (0 flat -- every reader keeps the flat surface; 1 low, 2 high), y storm 0..1,
//           z Beaufort 0..12, w detail-normal strength (1 = LL's normal maps as they were)
//   sea[1]  xy wave travel direction (unit, region axes), z eased mean wind (m/s), w visible Hs (m)
//   sea[2]  x surf 0..1 (breaker strength), y run-up (m above the water line), z swash phase 0..1,
//           w breaker spacing toward the shore (m)
//   sea[3]  xy the field's texel (0, 0) corner (agent m), z 1 / texel (1/m), w the surface height at
//           the main camera above the water plane (m: the CPU's under-water test used it)
//   sea[4]  x field resolution (texels per side), y 1 = the field is bound and valid,
//           z the field's first texel in binding 67, w chop (sum Q k A of the open sea)
//   sea[5]  the water VS's camera-centred grid: x quads per ring side, y rings, z the finest
//           spacing (m), w the outermost ring's reach (m)
//   sea[6]  xy the main camera's horizontal forward (unit; the VS draws its grid far to near)
//   sea_wave[2i]   xy wave vector (rad/m), z amplitude (m, deep open water), w phase (rad)
//   sea_wave[2i+1] x Gerstner Q, y k (rad/m), z omega (rad/s), w 0
// SEA FIELD (draw set 0 binding 67, std430 readonly): per texel uvec2, x = half2(depth, shore),
//   y = half2(fetch, dh): water depth (m, < 0 on land), signed shore distance (m, + offshore),
//   open water upwind (m), the texel's region water height - waterHeight() (m). Row-major,
//   sea[4].x per row, starting at texel sea[4].z. Outside the window: open deep sea.
#ifndef CLEAN_SEA_STATE_GLSL
#define CLEAN_SEA_STATE_GLSL
#extension GL_EXT_control_flow_attributes : require   // [[unroll]]: the wave arrays stay in registers

#define SEA_WAVES 8

layout(std430, set = 0, binding = 67) readonly buffer SeaFieldBuf { uvec2 sea_field[]; };

bool  seaOn()       { return ssr.sea[0].x > 0.5; }
// (the weather half -- storm, Beaufort, wind, swash clock -- is filled on flat water too: the foam)
float seaStorm()    { return ssr.sea[0].y; }
float seaBeaufort() { return ssr.sea[0].z; }
float seaDetail()   { return ssr.sea[0].w; }
vec2  seaWindDir()  { return ssr.sea[1].xy; }   // the waves' travel = where the wind blows TO
float seaWindSpeed(){ return ssr.sea[1].z; }
float seaHs()       { return ssr.sea[1].w; }
float seaSurf()     { return ssr.sea[2].x; }
float seaSwashPhase() { return ssr.sea[2].z; }
float seaCamLift()  { return ssr.sea[3].w; }

// ---- the field ----
struct SeaTexel
{
    float depth;    // water depth (m; < 0 on land)
    float shore;    // signed shore distance (m; + offshore)
    float fetch;    // open water upwind (m)
    float dh;       // region water height offset (m)
    vec2  shoreN;   // d shore / d xy (per m): the offshore direction (unnormalized)
};
SeaTexel seaOpenSea()
{
    SeaTexel t;
    t.depth = 100.0; t.shore = 1000.0; t.fetch = 4000.0; t.dh = 0.0; t.shoreN = vec2(0.0);
    return t;
}
vec4 seaTexelRaw(ivec2 c, int res)
{
    const uvec2 u = sea_field[uint(ssr.sea[4].z + 0.5) + uint(c.y * res + c.x)];
    return vec4(unpackHalf2x16(u.x), unpackHalf2x16(u.y));
}
// Bilinear over the window; open deep sea outside it (blended over the last texel).
SeaTexel seaField(vec2 xy)
{
    SeaTexel t = seaOpenSea();
    if (ssr.sea[4].y < 0.5) return t;
    const int res = int(ssr.sea[4].x + 0.5);
    const vec2 g = (xy - ssr.sea[3].xy) * ssr.sea[3].z - 0.5;
    const vec2 fl = floor(g);
    const ivec2 c0 = ivec2(fl);
    if (any(lessThan(c0, ivec2(0))) || any(greaterThanEqual(c0 + 1, ivec2(res)))) return t;
    const vec2 f = g - fl;
    const vec4 a = seaTexelRaw(c0, res), b = seaTexelRaw(c0 + ivec2(1, 0), res);
    const vec4 c = seaTexelRaw(c0 + ivec2(0, 1), res), d = seaTexelRaw(c0 + ivec2(1, 1), res);
    const vec4 v = mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
    t.depth = v.x; t.shore = v.y; t.fetch = v.z; t.dh = v.w;
    t.shoreN = vec2(mix(b.y - a.y, d.y - c.y, f.y), mix(c.y - a.y, d.y - b.y, f.x)) * ssr.sea[3].z;
    // fade to the open sea over the window's outer 8 texels (a camera far from the field's
    // centre never sees a seam where the window ends)
    const float e = min(min(g.x, g.y), float(res - 2) - max(g.x, g.y));
    const float w = clamp(e / 8.0, 0.0, 1.0);
    if (w < 1.0)
    {
        const SeaTexel o = seaOpenSea();
        t.depth = mix(o.depth, t.depth, w); t.shore = mix(o.shore, t.shore, w);
        t.fetch = mix(o.fetch, t.fetch, w); t.dh = mix(0.0, t.dh, w); t.shoreN *= w;
    }
    return t;
}

// ---- the surface ----
struct SeaSurf
{
    vec3  disp;       // displacement of the flat point (xy, waterHeight() + dh) to the surface (m)
    vec3  n;          // unit surface normal (the waves + the breakers; no detail normals)
    vec2  slope;      // n.xy / n.z: what a detail normal is tilted by
    float crest;      // 0..1 folding crest (the Gerstner Jacobian below ~0.75): whitecap / crest foam
    float breaking;   // 0..1 waves breaking on the shoaling bottom + the breaker fronts at a beach
    float swash;      // 0..1 the swash film rushing up the beach (thin water over sand)
    float depth;      // the field's water depth here (m; < 0 on land)
    float dh;         // the field's region water height offset (m)
};

SeaSurf seaFlatSurface()
{
    SeaSurf s;
    s.disp = vec3(0.0); s.n = vec3(0.0, 0.0, 1.0); s.slope = vec2(0.0);
    s.crest = 0.0; s.breaking = 0.0; s.swash = 0.0; s.depth = 100.0; s.dh = 0.0;
    return s;
}

// Breaker profile over its phase psi (0..1): a steep front, a long decaying back.
float seaBore(float psi) { return smoothstep(0.0, 0.07, psi) * pow(1.0 - psi, 2.5); }

// The surface at the flat point xy (agent m). camDist: the main camera's horizontal distance (m)
// for the geometry LOD fade (each wave fades out by 4 of its wavelengths, the grid's resolution
// there; < 0 = no fade). footprint: the pixel footprint (m) for the shading's normal filter (each
// wave's slope fades below ~3 pixels per wavelength; 0 = none).
SeaSurf seaSurface(vec2 xy, float camDist, float footprint)
{
    SeaSurf s = seaFlatSurface();
    if (!seaOn()) return s;
    const SeaTexel f = seaField(xy);
    s.depth = f.depth; s.dh = f.dh;
    const float d = max(f.depth, 0.0);

    // Per wave: the fetch gate (a wave needs ~3-12 wavelengths of open water upwind to build),
    // shoaling (taller + steeper as the bottom rises: k d < ~1), then the depth cap below.
    float A[SEA_WAVES], Q[SEA_WAVES];
    float aSum = 0.0;
    float e2 = 0.0, o2 = 0.0;   // the fetch-gated / open amplitudes' energies (the breakers' strength)
    [[unroll]] for (int i = 0; i < SEA_WAVES; ++i)
    {
        const float k = ssr.sea_wave[2 * i + 1].y;
        const float lam = 6.2831853 / max(k, 1e-4);
        const float gate = smoothstep(3.0 * lam, 12.0 * lam, f.fetch);
        const float a0 = ssr.sea_wave[2 * i].z;
        e2 += a0 * gate * a0 * gate;
        o2 += a0 * a0;
        const float th = 1.0 - 2.0 / (exp(2.0 * min(k * d, 8.0)) + 1.0);   // tanh(k d)
        A[i] = ssr.sea_wave[2 * i].z * gate * (1.0 + 0.6 * (1.0 - th));
        Q[i] = ssr.sea_wave[2 * i + 1].x * (1.0 + 1.5 * (1.0 - th));
        aSum += A[i];
    }
    // DEPTH CAP: a wave higher than ~0.8 x the depth breaks (H = 2 A <= 0.84 d): the cap's bite
    // is the breaking mask; on land every wave is gone.
    const float aMax = 0.42 * d;
    const float over = aSum / max(aMax, 1e-3);
    s.breaking = f.depth > 0.0 ? smoothstep(0.85, 1.4, over) * smoothstep(0.0, 0.05, aSum) : 0.0;
    const float cap = aSum > aMax ? aMax / max(aSum, 1e-6) : 1.0;
    float qs = 0.0;
    [[unroll]] for (int i = 0; i < SEA_WAVES; ++i)
    {
        const float k = ssr.sea_wave[2 * i + 1].y;
        const float lam = 6.2831853 / max(k, 1e-4);
        A[i] *= cap;
        if (camDist >= 0.0) A[i] *= 1.0 - smoothstep(2.5 * lam, 4.0 * lam, camDist);
        qs += Q[i] * k * A[i];
    }
    const float qn = qs > 0.95 ? 0.95 / qs : 1.0;

    // Gerstner sum + its partial derivatives (dP/dx0, dP/dy0) for the normal and the Jacobian.
    float jxx = 1.0, jyy = 1.0, jxy = 0.0;
    vec2 dz = vec2(0.0);
    [[unroll]] for (int i = 0; i < SEA_WAVES; ++i)
    {
        const vec4 w0 = ssr.sea_wave[2 * i];
        const float k = ssr.sea_wave[2 * i + 1].y;
        const vec2 dir = w0.xy / max(k, 1e-4);
        const float th = dot(w0.xy, xy) + w0.w;
        const float sn = sin(th), cs = cos(th);
        const float a = A[i], qa = Q[i] * qn * a;
        s.disp.xy -= dir * (qa * sn);
        s.disp.z  += a * cs;
        // shading filter: slopes of waves shorter than ~3 footprints fade (no sparkle)
        const float lam = 6.2831853 / max(k, 1e-4);
        const float nf = footprint > 0.0 ? 1.0 - smoothstep(lam * 0.17, lam * 0.33, footprint) : 1.0;
        const float ka = k * a * nf, kqa = k * qa * nf;
        dz   -= dir * (ka * sn);
        jxx  -= dir.x * dir.x * kqa * cs;
        jyy  -= dir.y * dir.y * kqa * cs;
        jxy  -= dir.x * dir.y * kqa * cs;
    }
    const float J = jxx * jyy - jxy * jxy;
    s.crest = smoothstep(0.75, 0.25, J);

    // BREAKERS AT THE SHORE: bores running in toward decreasing shore distance (one every
    // swashPeriod, sea[2].w apart), lifting the surface by the breaker height offshore and by the
    // run-up on the beach -- the lifted water shows wherever it clears the sand (the swash film),
    // and recedes as the bore's back decays.
    // STRENGTH (#1020 river fix; slseamath.h slsea::boreStrength is the CPU twin): the global surf x
    // the LOCAL wave energy (the fetch-gated amplitudes as a fetch-limited Hs over the open sea's) x
    // how squarely the shore faces the incoming waves (a soft floor for refraction; a lee shore:
    // none). A channel narrower than two surf zones was all surf zone: its shore-distance ridge
    // (the centre line) started bores running out to both banks. Now it laps.
    float surf = 0.0;
    {
        const float energy = o2 > 1e-12 ? min(sqrt(e2 / o2), 1.0) : 0.0;
        const float l = length(f.shoreN);
        const float facing = l > 1e-4 ? -dot(seaWindDir(), f.shoreN) / l : -1.0;
        const float kFacingFloor = 0.15;
        surf = seaSurf() * energy * clamp((facing + kFacingFloor) / (1.0 + kFacingFloor), 0.0, 1.0);
    }
    const float W = clamp(8.0 * seaHs() + 6.0, 6.0, 40.0);   // surf zone width offshore (m)
    const float Rl = 15.0;                                     // swash reach inland (m)
    vec2 dzBore = vec2(0.0);
    if (surf > 0.0 && f.shore < W && f.shore > -Rl)
    {
        const float L = max(ssr.sea[2].w, 1.0);
        const float psi = fract(ssr.sea[2].z + f.shore / L);
        const float b = seaBore(psi);
        const float runup = ssr.sea[2].y;
        const float hb = min(0.45 * d + 0.05, 0.6 * seaHs() + 0.05);
        const float E = f.shore >= 0.0
            ? mix(runup, hb, smoothstep(0.0, 0.35 * W, f.shore)) * (1.0 - smoothstep(0.35 * W, W, f.shore))
            : runup * smoothstep(-Rl, 0.0, f.shore);
        const float lift = surf * b * E;
        // GEOMETRY LOD (#1020 jitter): the bore's front is ~0.07 L wide; a grid coarser than that
        // (far rings) samples it a different way every frame as psi runs -- the far shoreline's
        // mesh jumped. The displacement fades out past 1 .. 2 breaker spacings (the grid still
        // resolves the front there); the shading keeps the masks, its slope filtered below.
        const float liftGeo = camDist >= 0.0 ? lift * (1.0 - smoothstep(L, 2.0 * L, camDist)) : lift;
        s.disp.z += liftGeo;
        // its slope along the shore normal (numeric d b / d psi; psi grows with the shore distance),
        // faded as the waves' slopes below ~3 footprints per front
        const float db = (seaBore(min(psi + 0.01, 1.0)) - b) / 0.01;
        const float nfb = footprint > 0.0 ? 1.0 - smoothstep(0.03 * L, 0.1 * L, footprint) : 1.0;
        dzBore = f.shoreN * (surf * E * db / L) * nfb * (liftGeo / max(lift, 1e-6));
        dz += dzBore;
        const float front = smoothstep(0.0, 0.05, psi) * (1.0 - smoothstep(0.07, 0.3, psi));
        s.breaking = max(s.breaking, surf * front * (1.0 - smoothstep(0.35 * W, W, max(f.shore, 0.0))));
        // the film: thin water over sand where the lifted surface clears the ground
        s.swash = smoothstep(0.0, 0.08, f.depth + lift) * (1.0 - smoothstep(-0.1, 0.5, f.depth)) * smoothstep(0.0, 0.02, lift);
    }

    // normal = cross(dP/dx0, dP/dy0); dP/dx0 = (jxx, jxy, dz.x), dP/dy0 = (jxy, jyy, dz.y)
    const vec3 tx = vec3(jxx, jxy, dz.x), ty = vec3(jxy, jyy, dz.y);
    vec3 n = cross(tx, ty);
    n = n.z > 1e-3 ? n : vec3(-dz, 1.0);   // (a folded crest: the plain slope)
    s.n = normalize(n);
    s.slope = s.n.xy / max(s.n.z, 0.2);
    return s;
}

// Height of the surface above the flat plane, seen at a fixed point xy (no horizontal shift
// inverted): the RT forward pass's ray march, a cheap test. Matches SLSeaState::surfaceHeightAt.
float seaHeightAt(vec2 xy) { return seaSurface(xy, -1.0, 0.0).disp.z; }

#endif // CLEAN_SEA_STATE_GLSL
