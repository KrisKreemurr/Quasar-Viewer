// Shared volumetric field accessors. Physical mode reads this eye's immutable
// camera-medium parameters; legacy mode retains the original extra-fog contract.
// Physical spatial mapping is cmDepth/cmSlice with the reserved start boundary;
// the unwarped volDepthOfSlice/volSliceOfDepth helpers below are legacy-only.
#ifndef CLEAN_VOL_COMMON_GLSL
#define CLEAN_VOL_COMMON_GLSL

mat4 volVP() { return cmOn() ? ssr.medium[MEDIUM_EYE].vp : ssr.vol_vp; }
mat4 volInvVP() { return cmOn() ? ssr.medium[MEDIUM_EYE].inv_vp : ssr.vol_inv_vp; }
vec4 volEye() { return cmOn() ? ssr.medium[MEDIUM_EYE].eye : ssr.vol_eye; }
vec4 volGrid() { return cmOn() ? ssr.medium[MEDIUM_EYE].grid : ssr.vol_grid; }
vec4 volSlice() { return cmOn() ? ssr.medium[MEDIUM_EYE].slice : ssr.vol_slice; }
vec4 volFog() { return cmOn() ? ssr.medium[MEDIUM_EYE].fog : ssr.vol_fog; }
vec4 volAlbedo() { return cmOn() ? ssr.medium[MEDIUM_EYE].albedo : ssr.vol_albedo; }
vec4 volMisc() { return cmOn() ? ssr.medium[MEDIUM_EYE].misc : ssr.vol_misc; }
bool  volOn()       { return volEye().w > 0.5; }
float volFar()      { return volGrid().w; }
uvec3 volDims()     { return uvec3(volGrid().xyz + 0.5); }

// Slice coordinate u in [0, 1] <-> view depth (m).
float volDepthOfSlice(float u) { return volSlice().x * (exp(u * volSlice().y) - 1.0); }
float volSliceOfDepth(float vz) { return log(1.0 + max(vz, 0.0) / volSlice().x) / volSlice().y; }

// Henyey-Greenstein phase; cos_t = dot(view direction (eye -> point), direction TO the light).
float volPhaseHG(float cos_t, float g)
{
    const float g2 = g * g;
    return (1.0 - g2) / (4.0 * ATMOS_PI * pow(max(1.0 + g2 - 2.0 * g * cos_t, 1e-4), 1.5));
}

// The sun / moon beam (linear): the haze light colour through the atmosphere above,
// as clean_godrays.comp beamColor() (atmos_k[0] is the same extinction, #816).
vec3 volBeam() { return atmosSrgbToLinear(ssr.atmos_k[0].rgb) * ssr.atmos[12].z; }

// Sky ambient radiance (linear), as atmosLightTerms' amblit with the normal factor
// averaged out (a medium has no normal), x the fog's ambient scale.
vec3 volAmbient()
{
    vec3 a = ssr.atmos_k[4].rgb * 0.9;   // (atmosAmbientLighting's mean over the sphere ~0.9)
    if (!atmosClassic())
    {
        a = atmosSrgbToLinear(a);
        a = vec3(dot(a, vec3(0.2126, 0.7152, 0.0722)));
    }
    return a * ssr.atmos[12].w * volFog().w;
}

// The WindLight haze's extinction (/m), LL's atten.r rate (atmos_k[1].r); 0 with the
// atmosphere off.
float volHazeExt() { return !cmOn() && atmosEnabled() ? ssr.atmos_k[1].r : 0.0; }

// The extra height fog's extinction at world position P (/m): sigma0 at and below the
// base height, decaying exp(-k (z - base)) above it; none below the water cut (the water
// fog owns everything under the surface).
float volFogDensity(vec3 P)
{
    if (P.z < volMisc().z) return 0.0;
    return volFog().x * exp(-volFog().y * max(P.z - volFog().z, 0.0));
}

// Optical depth of the height fog along the segment A -> B (analytic: the exponential
// integral; the segment is clipped to the water cut first, and the base-height clamp is
// applied at the two ends -- exact for segments wholly above the base, a close bound
// for the rest).
float volFogOpticalDepth(vec3 A, vec3 B)
{
    const float s0 = volFog().x;
    if (s0 <= 0.0) return 0.0;
    const float k = volFog().y, base = volFog().z, cut = volMisc().z;
    float za = A.z, zb = B.z;
    float len = length(B - A);
    if (za < cut && zb < cut) return 0.0;
    if (za < cut || zb < cut)
    {
        const float f = (cut - za) / (zb - za);   // (za != zb here)
        if (za < cut) { len *= 1.0 - f; za = cut; } else { len *= f; zb = cut; }
    }
    const float ea = max(za - base, 0.0), eb = max(zb - base, 0.0);
    const float x = k * (eb - ea);
    const float avg = abs(x) < 1e-4 ? exp(-k * ea) * (1.0 - 0.5 * x)
                                    : (exp(-k * ea) - exp(-k * eb)) / x;
    return s0 * len * avg;
}

// The extra fog's UNSHADOWED source radiance per unit extinction along view direction V:
// albedo x (pi x beam x phase (sun above the horizon) + ambient). pi: LL's light colours are
// irradiance / pi (a Lambertian surface shows albedo x colour x N.L), so a medium scatters
// sigma_s x pi x colour x phase -- the same light the surfaces get, in the same units.
vec3 volFogSource(vec3 V)
{
    const vec3 L = atmosLightDir();
    vec3 sun = vec3(0.0);
    if (L.z > 0.0) sun = ATMOS_PI * volBeam() * volPhaseHG(dot(V, L), volSlice().w);
    return volAlbedo().rgb * (sun + volAmbient());
}

#endif
