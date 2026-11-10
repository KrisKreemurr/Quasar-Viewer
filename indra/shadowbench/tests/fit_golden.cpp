// Build (from indra/): g++ -std=c++20 -O2 -march=x86-64-v2 -DLL_LINUX=1 -I llcommon -I llmath -I newview/vkgfx -I <build>/packages/include \
//   shadowbench/tests/fit_golden.cpp newview/vkgfx/llvkshadowcascades.cpp
// GOLDEN: the cascade fit as it was in LLAppViewer (9cc7f58542, llappviewer.cpp 7909-8046,
// verbatim but for the settings / singletons named as parameters) vs vkShadowFitCascades.
#include "linden_common.h"
#include "v3math.h"
#include "llvkshadowcascades.h"
#include <cstdio>
#include <cstring>
#include <random>
struct WF { float shadow_mat[4][16]; float shadow_planes[4][6][4]; float shadow_clip[4]; float shadow_texel[4];
            float shadow_slice[4][4]; unsigned shadow_update; float shadow_caster_planes[4][6][4]; };
static void origFit(WF& wf, LLVector3 L, LLVector3 eye, LLVector3 at, LLVector3 left, LLVector3 up, F32 zn, F32 render_far,
                    F32 range_scale, LLVector3 split, U32 upd, U32 res_alloc_in, F32 fovY, F32 aspect, bool ortho,
                    F32 ortho_hw, F32 ortho_hh)
{
            const F32 n = llclamp(zn, 0.1f, 4.f);
            // (VRAM ladder "far shadows off", #405: the range shrinks; settings untouched.)
            const F32 f = llmax(n + 16.f, llmin(render_far, 512.f) * range_scale);
            const LLVector3 sx = split;
            const F32 da = powf(1.f - llmax(fabsf(L * up), fabsf(L * left)), sx.mV[2]);
            const F32 sxp = sx.mV[1] + (sx.mV[0] - sx.mV[1]) * da;
            F32 clip[4];
            for (int i = 0; i < 4; ++i) clip[i] = n + (f - n) * powf((F32)(i + 1) / 4.f, sxp);
            clip[0] *= 1.25f;

            // Light basis: fw = direction light travels (ortho forward), rt/uu span the map.
            wf.shadow_update = upd;
            const LLVector3 fw = -L;
            LLVector3 rt = (fabsf(fw.mV[VZ]) < 0.99f ? LLVector3(0.f, 0.f, 1.f) : LLVector3(1.f, 0.f, 0.f)) % fw;
            rt.normalize();
            const LLVector3 uu = fw % rt;
            const F32 k = tanf(fovY * 0.5f) * sqrtf(1.f + aspect * aspect);   // half-diagonal slope
            // The map size actually allocated (a #226 fit cap / the VRAM ladder can hold it
            // below the setting, #405); the setting until the renderer has one.
            const U32 res_alloc = res_alloc_in;
            const U32 res = res_alloc ? res_alloc : llclamp(2048u, 256u, 8192u);
            // Capped at 2048 m (unchanged for draw distances <= 1024): at a 4 km draw
            // distance a 8 km pull-back would drag skyboxes kilometres overhead into the
            // ground cascades (and the caster cull) while the shadow range stays 512 m.
            const F32 back = llclamp(2.f * render_far, 512.f, 2048.f);
            for (int c = 0; c < 4; ++c)
            {
                const F32 a = (c == 0) ? n : clip[c - 1] * 0.75f;
                const F32 b = (c == 3) ? clip[3] : clip[c] * 1.25f;
                // Minimal sphere around the slice's 8 corners (centre on the view axis).
                F32 t = llmin(b, (a + b) * (1.f + k * k) * 0.5f);
                F32 R = sqrtf(llmax((b - t) * (b - t) + b * b * k * k, (t - a) * (t - a) + a * a * k * k));
                if (ortho)   // the box slice: the same half diagonal at both ends
                {
                    t = 0.5f * (a + b);
                    R = sqrtf(0.25f * (b - a) * (b - a) + ortho_hw * ortho_hw + ortho_hh * ortho_hh);
                }
                const LLVector3 ctr = eye + at * t;
                wf.shadow_slice[c][0] = ctr.mV[0]; wf.shadow_slice[c][1] = ctr.mV[1];
                wf.shadow_slice[c][2] = ctr.mV[2]; wf.shadow_slice[c][3] = R;
                // Cached cascades (all but the nearest, when shadow updates aren't
                // realtime) are fitted 15% larger, so the map stays valid while the
                // camera moves until the renderer's coverage test says re-render.
                if (wf.shadow_update > 0 && c > 0) R *= 1.15f;
                R = ceilf(R * 4.f) * 0.25f;
                const F32 texel = 2.f * R / (F32)res;
                // TEXEL SNAP in ABSOLUTE light space (the grid is fixed to the world, so
                // the map is stable under camera motion), in double: every input is a
                // float, so the products are exact and floor() sees the true quotient --
                // at Z 3000 the float quotient (~1e6 texels) was itself off by ~0.03 texel.
                auto dotd = [](const LLVector3& a, const LLVector3& b)
                { return (F64)a.mV[0] * b.mV[0] + (F64)a.mV[1] * b.mV[1] + (F64)a.mV[2] * b.mV[2]; };
                const F64 texd = (F64)texel;
                const F64 cxd = std::floor(dotd(ctr, rt) / texd) * texd;
                const F64 cyd = std::floor(dotd(ctr, uu) / texd) * texd;
                const F32 cx = (F32)cxd;
                const F32 cy = (F32)cyd;
                const F32 cz = ctr * fw;
                const F32 znear = cz - R - back, zfar = cz + R;
                const F32 iz = 1.f / (zfar - znear);
                // World -> shadow NDC (x,y in [-1,1] map to uv like the VK viewport;
                // z in [0,1]), column-major. Rendered and sampled with the same matrix,
                // so no Y flip is involved. Mw (world space) only feeds the cull planes.
                F32 Mw[16];
                Mw[0] = rt.mV[0] / R; Mw[4] = rt.mV[1] / R; Mw[8]  = rt.mV[2] / R; Mw[12] = -cx / R;
                Mw[1] = uu.mV[0] / R; Mw[5] = uu.mV[1] / R; Mw[9]  = uu.mV[2] / R; Mw[13] = -cy / R;
                Mw[2] = fw.mV[0] * iz; Mw[6] = fw.mV[1] * iz; Mw[10] = fw.mV[2] * iz; Mw[14] = -znear * iz;
                Mw[3] = 0.f; Mw[7] = 0.f; Mw[11] = 0.f; Mw[15] = 1.f;
                // CAMERA-RELATIVE (#555): what the GPU gets maps (world - eye), the
                // translation built in double (the snapped centre minus the eye, both
                // ~km along the light axes, cancel exactly here instead of per vertex /
                // per pixel in float). World-space, at Z 3000 the receiver depth and the
                // caster depth each carried ~2e-4 m of float noise -- 3x the D32
                // constant bias -- which re-rounded every frame: flickering acne on
                // sun-facing floors (normal offset ~0 there). Camera-relative: ~2e-5 m,
                // the same as at ground level.
                {
                    const F64 zneard = dotd(ctr - eye, fw) - (F64)R - (F64)back;   // znear - eye.fw
                    F32* M = wf.shadow_mat[c];
                    std::memcpy(M, Mw, sizeof(Mw));
                    M[12] = (F32)(-(cxd - dotd(eye, rt)) / (F64)R);
                    M[13] = (F32)(-(cyd - dotd(eye, uu)) / (F64)R);
                    M[14] = (F32)(-zneard * (F64)iz);
                }
                const F32* M = Mw;
                // Cull planes (Gribb-Hartmann, VK depth 0..1): L R B T near far.
                F32 rw[4][4];
                for (int r = 0; r < 4; ++r) for (int col = 0; col < 4; ++col) rw[r][col] = M[col * 4 + r];
                const int sgn[6][2] = {{0,1},{0,-1},{1,1},{1,-1},{2,0},{2,-1}};
                for (int i = 0; i < 6; ++i)
                {
                    F32 pl[4];
                    for (int col = 0; col < 4; ++col)
                        pl[col] = (i == 4) ? rw[2][col] : rw[3][col] + (F32)sgn[i][1] * rw[sgn[i][0]][col];
                    const F32 len = sqrtf(pl[0] * pl[0] + pl[1] * pl[1] + pl[2] * pl[2]);
                    for (int col = 0; col < 4; ++col) wf.shadow_planes[c][i][col] = len > 0.f ? pl[col] / len : pl[col];
                }
                wf.shadow_clip[c]  = clip[c];
                wf.shadow_texel[c] = texel;

                // SHADOW-CASTER PLANES for this cascade's view slice [a, b]: a shadow
                // travels along fw, so for every slice plane whose inward normal does
                // not point along fw (n.fw <= 0) the caster itself must already be
                // inside it. Necessary, not sufficient -- conservative by construction.
                {
                    const F32 hy = fovY * 0.5f;
                    const F32 hx = atanf(tanf(hy) * aspect);
                    LLVector3 nrm[6];
                    F32 dd[6];
                    auto side = [&](const LLVector3& edge_dir, const LLVector3& axis, int i) {
                        LLVector3 nn = axis % edge_dir;
                        if (nn * at < 0.f) nn = -nn;
                        nn.normalize();
                        nrm[i] = nn; dd[i] = -(nn * eye);
                    };
                    side(at * cosf(hx) + left * sinf(hx), up,   0);   // left
                    side(at * cosf(hx) - left * sinf(hx), up,   1);   // right
                    side(at * cosf(hy) + up * sinf(hy),   left, 2);   // top
                    side(at * cosf(hy) - up * sinf(hy),   left, 3);   // bottom
                    if (ortho)   // the box's sides: parallel to the view axis
                    {
                        nrm[0] = -left; dd[0] = ortho_hw + left * eye;   // left
                        nrm[1] =  left; dd[1] = ortho_hw - left * eye;   // right
                        nrm[2] = -up;   dd[2] = ortho_hh + up * eye;     // top
                        nrm[3] =  up;   dd[3] = ortho_hh - up * eye;     // bottom
                    }
                    nrm[4] = at;  dd[4] = -(at * eye) - a;            // slice near
                    nrm[5] = -at; dd[5] =  (at * eye) + b;            // slice far
                    for (int i = 0; i < 6; ++i)
                    {
                        F32* P = wf.shadow_caster_planes[c][i];
                        if (nrm[i] * fw <= 0.f)
                        { P[0] = nrm[i].mV[0]; P[1] = nrm[i].mV[1]; P[2] = nrm[i].mV[2]; P[3] = dd[i]; }
                        else
                        { P[0] = 0.f; P[1] = 0.f; P[2] = 0.f; P[3] = 1.f; }
                    }
                }
            }
}
int main()
{
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> u(-1.f, 1.f);
    int bad = 0, n = 0;
    for (int it = 0; it < 200000; ++it)
    {
        LLVector3 L(u(rng), u(rng), std::fabs(u(rng)) + 0.05f); L.normalize();
        LLVector3 at(u(rng), u(rng), 0.5f * u(rng)); at.normalize();
        LLVector3 left = LLVector3(0, 0, 1) % at; left.normalize();
        LLVector3 up = at % left; up.normalize();
        LLVector3 eye(128.f + 300.f * u(rng), 128.f + 300.f * u(rng), 30.f + (it % 7 == 0 ? 3000.f : 200.f) * std::fabs(u(rng)));
        const F32 zn = 0.1f + 2.f * std::fabs(u(rng));
        const F32 rf = (it % 3 == 0) ? 64.f + 4000.f * std::fabs(u(rng)) : 256.f;
        const F32 rs = (it % 5 == 0) ? 0.5f : 1.f;
        const U32 upd = it % 3;
        const U32 res = (it % 4 == 0) ? 1024u : 2048u;
        const F32 fov = 0.6f + std::fabs(u(rng)), asp = 1.f + std::fabs(u(rng));
        const bool ortho = (it % 11 == 0);
        const F32 ohw = 10.f + 50.f * std::fabs(u(rng)), ohh = 10.f + 50.f * std::fabs(u(rng));
        WF wf{};
        origFit(wf, L, eye, at, left, up, zn, rf, rs, LLVector3(3.f, 3.f, 2.f), upd, res, fov, asp, ortho, ohw, ohh);
        VKShadowFitIn in;
        std::memcpy(in.eye, eye.mV, 12); std::memcpy(in.at, at.mV, 12); std::memcpy(in.left, left.mV, 12);
        std::memcpy(in.up, up.mV, 12); std::memcpy(in.light, L.mV, 12);
        in.fov_y = fov; in.aspect = asp; in.near_clip = zn; in.range = llmin(rf, 512.f) * rs; in.render_far = rf;
        in.res = res; in.ortho = ortho; in.ortho_hw = ohw; in.ortho_hh = ohh;
        for (int c = 1; c < 4; ++c) in.scale[c] = upd > 0 ? 1.15f : 1.f;
        VKShadowFitOut o{};
        vkShadowFitCascades(in, o);
        ++n;
        if (std::memcmp(o.mat, wf.shadow_mat, sizeof(o.mat)) || std::memcmp(o.planes, wf.shadow_planes, sizeof(o.planes))
            || std::memcmp(o.clip, wf.shadow_clip, sizeof(o.clip)) || std::memcmp(o.texel, wf.shadow_texel, sizeof(o.texel))
            || std::memcmp(o.slice, wf.shadow_slice, sizeof(o.slice))
            || std::memcmp(o.caster_planes, wf.shadow_caster_planes, sizeof(o.caster_planes)))
        {
            if (bad < 5) std::printf("MISMATCH it %d (mat %d planes %d clip %d texel %d slice %d caster %d)\n", it,
                std::memcmp(o.mat, wf.shadow_mat, sizeof(o.mat)) != 0, std::memcmp(o.planes, wf.shadow_planes, sizeof(o.planes)) != 0,
                std::memcmp(o.clip, wf.shadow_clip, sizeof(o.clip)) != 0, std::memcmp(o.texel, wf.shadow_texel, sizeof(o.texel)) != 0,
                std::memcmp(o.slice, wf.shadow_slice, sizeof(o.slice)) != 0,
                std::memcmp(o.caster_planes, wf.shadow_caster_planes, sizeof(o.caster_planes)) != 0);
            ++bad;
        }
    }
    std::printf("fit golden: %d / %d bit-identical\n", n - bad, n);
    return bad ? 1 : 0;
}
