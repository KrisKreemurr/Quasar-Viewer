// shadowbench -- GPU runs: every (scenario, policy) pair, interleaved A B A B ... over reps,
// timing each shadow pass and diffing the receiver shadow image against the baseline policy.
//
//   shadowbench [--scenarios a,b] [--policies p,q] [--reps N] [--seconds S] [--filter Q]
//               [--res N] [--validate] [--list]
#include "sb_gpu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>

namespace
{
std::vector<SbPolicy> policyTable()
{
    std::vector<SbPolicy> t;
    auto add = [&](const char* name) -> SbPolicy& { t.push_back(SbPolicy{}); t.back().name = name; return t.back(); };
    add("realtime").mode = 0;                                    // Ultra / the shipped default today
    add("realtime_b").mode = 0;                                  // (the same again: the run-to-run noise floor)
    { SbPolicy& p = add("cached"); p.mode = 1; p.roll = true; p.static_slice_planes = true; }  // RenderVKShadowUpdate 1 as it was (9cc7f58542)
    { SbPolicy& p = add("cached_all"); p.mode = 1; p.force_all = true; }   // cached structure, every cascade every frame
    { SbPolicy& p = add("cached_all_ns"); p.mode = 1; p.force_all = true; p.scale_n = 1.f; }   // ... without the 15 % slack
    { SbPolicy& p = add("cached_all_ns_t"); p.mode = 1; p.force_all = true; p.scale_n = 1.f; p.trans_ovl = true; }
    { SbPolicy& p = add("cached_trans"); p.mode = 1; p.trans_ovl = true; p.roll = true; p.static_slice_planes = true; }  // + translucency overlay
    // THE NEW PATH: translucency overlay + border (realtime texels) + swept caster volume + no roll term
    { SbPolicy& p = add("new"); p.mode = 1; p.trans_ovl = true; p.scale_n = 1.f; p.border_frac = 0.15f; p.swept = true; }
    { SbPolicy& p = add("new_ssp"); p.mode = 1; p.trans_ovl = true; p.scale_n = 1.f; p.border_frac = 0.15f; p.swept = true; p.static_slice_planes = true; }
    { SbPolicy& p = add("new_n1"); p.mode = 1; p.trans_ovl = true; p.scale_n = 1.f; p.border_frac = 0.15f; p.swept = true; p.far_n = 1; p.defer = 0; }
    { SbPolicy& p = add("new_roll"); p.mode = 1; p.trans_ovl = true; p.scale_n = 1.f; p.border_frac = 0.15f; p.swept = true; p.roll = true; }
    { SbPolicy& p = add("new_noswept"); p.mode = 1; p.trans_ovl = true; p.scale_n = 1.f; p.border_frac = 0.15f; }
    { SbPolicy& p = add("new_l2r64"); p.mode = 1; p.trans_ovl = true; p.scale_n = 1.f; p.border_frac = 0.15f; p.swept = true; p.lever = 2; }
    { SbPolicy& p = add("lever2r"); p.mode = 1; p.trans_ovl = true; p.roll = false; p.lever = 1; }
    { SbPolicy& p = add("lever2r64"); p.mode = 1; p.trans_ovl = true; p.roll = false; p.lever = 2; }
    { SbPolicy& p = add("noslack"); p.mode = 1; p.trans_ovl = true; p.roll = false; p.lever = 2; p.scale_n = 1.f; }
    { SbPolicy& p = add("ns_t"); p.mode = 1; p.trans_ovl = true; p.scale_n = 1.f; }   // no slack + translucency overlay
    { SbPolicy& p = add("ns_l2r"); p.mode = 1; p.trans_ovl = true; p.scale_n = 1.f; p.roll = false; p.lever = 1; }
    { SbPolicy& p = add("ns_l2r64"); p.mode = 1; p.trans_ovl = true; p.scale_n = 1.f; p.roll = false; p.lever = 2; }
    { SbPolicy& p = add("ns_l2r_tx1"); p.mode = 1; p.trans_ovl = true; p.scale_n = 1.f; p.roll = false; p.lever = 1; p.light_tx = 1.f; }
    { SbPolicy& p = add("bd15"); p.mode = 1; p.trans_ovl = true; p.scale_n = 1.f; p.border_frac = 0.15f; }   // border = the 15 % slack, realtime texels
    { SbPolicy& p = add("bd15_all"); p.mode = 1; p.trans_ovl = true; p.scale_n = 1.f; p.border_frac = 0.15f; p.force_all = true; }
    { SbPolicy& p = add("bd15_l2r"); p.mode = 1; p.trans_ovl = true; p.scale_n = 1.f; p.border_frac = 0.15f; p.roll = false; p.lever = 1; }
    { SbPolicy& p = add("bd15_l2r64"); p.mode = 1; p.trans_ovl = true; p.scale_n = 1.f; p.border_frac = 0.15f; p.roll = false; p.lever = 2; }
    { SbPolicy& p = add("bd15_sw"); p.mode = 1; p.trans_ovl = true; p.scale_n = 1.f; p.border_frac = 0.15f; p.roll = false; p.lever = 2; p.swept = true; }
    { SbPolicy& p = add("rt_swept"); p.mode = 0; p.swept = true; }   // realtime + swept caster volume
    { SbPolicy& p = add("ct_swept"); p.mode = 1; p.trans_ovl = true; p.swept = true; }
    { SbPolicy& p = add("slack0"); p.mode = 1; p.trans_ovl = true; p.roll = false; p.lever = 2; p.scale0 = 1.06f; }
    return t;
}

std::vector<std::string> split(const std::string& s)
{
    std::vector<std::string> v; std::stringstream ss(s); std::string x;
    while (std::getline(ss, x, ',')) if (!x.empty()) v.push_back(x);
    return v;
}

double pct(std::vector<double> v, double p)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t i = std::min(v.size() - 1, (size_t)std::lround(p * (v.size() - 1)));
    return v[i];
}

struct Agg
{
    std::vector<double> run_mean, run_p50, run_p95, ref_p50;
    double cull = 0, draw = 0, trans = 0, copy = 0, dyncull = 0, ovl = 0, recv = 0;
    uint32_t nruns = 0;
    double renders_s[VKSC_CASCADES] = {}, draws[VKSC_CASCADES] = {};
    // image diff vs baseline (worst over runs)
    double dmax = 0, dmean = 0, p_any = 0, p_big = 0, tmax = 0;
    bool diffed = false;
};
}   // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> scen, pols;
    int reps = 2;
    std::string dump;
    SbOptions opt;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) { std::fprintf(stderr, "missing value\n"); std::exit(2); } return argv[++i]; };
        if (a == "--scenarios") scen = split(next());
        else if (a == "--policies") pols = split(next());
        else if (a == "--reps") reps = std::atoi(next().c_str());
        else if (a == "--seconds") opt.seconds = (float)std::atof(next().c_str());
        else if (a == "--filter") opt.filter = std::atoi(next().c_str());
        else if (a == "--res") opt.res = (uint32_t)std::atoi(next().c_str());
        else if (a == "--sample") opt.sample_every = std::atoi(next().c_str());
        else if (a == "--validate") opt.validate = true;
        else if (a == "--dump") dump = next();
        else if (a == "--list")
        {
            for (const SbPolicy& p : policyTable()) std::printf("policy %s\n", p.name.c_str());
            for (int s = 0; s < kSbScenarioCount; ++s) std::printf("scenario %s\n", kSbScenarios[s].name);
            return 0;
        }
        else { std::fprintf(stderr, "usage: shadowbench [--scenarios a,b] [--policies p,q] [--reps N] [--seconds S] [--filter Q] [--res N] [--validate] [--list]\n"); return 2; }
    }
    const std::vector<SbPolicy> table = policyTable();
    std::vector<SbPolicy> run_pols;
    if (pols.empty()) pols = {"realtime", "cached"};
    for (const std::string& n : pols)
    {
        auto it = std::find_if(table.begin(), table.end(), [&](const SbPolicy& p) { return p.name == n; });
        if (it == table.end()) { std::fprintf(stderr, "unknown policy %s\n", n.c_str()); return 2; }
        run_pols.push_back(*it);
    }
    std::vector<const SbScenario*> run_sc;
    for (int s = 0; s < kSbScenarioCount; ++s)
        if (scen.empty() || std::find(scen.begin(), scen.end(), kSbScenarios[s].name) != scen.end()) run_sc.push_back(&kSbScenarios[s]);

    VkDeviceContext ctx;
    ctx.init(opt.validate);
    SbBench bench;
    bench.init(ctx, SB_SHADER_DIR, opt);
    std::printf("[sb] device %s, shadow res %u, filter %d, receiver %ux%u, reps %d (ABAB interleaved)\n", bench.deviceName(), opt.res,
                opt.filter, opt.recv_w, opt.recv_h, reps);

    for (const SbScenario* sc : run_sc)
    {
        std::vector<Agg> agg(run_pols.size());
        std::vector<std::vector<uint16_t>> base_samples;
        for (int r = 0; r < reps; ++r)
            for (size_t pi = 0; pi < run_pols.size(); ++pi)
            {
                SbRunResult rr = bench.run(*sc, run_pols[pi]);
                Agg& A = agg[pi];
                std::vector<double> tot, ref;
                double cull = 0, draw = 0, trans = 0, copy = 0, dyn = 0, ovl = 0, recv = 0;
                for (const SbPassTimes& f : rr.frames)
                {
                    tot.push_back(f.shadow_total); ref.push_back(f.ref);
                    cull += f.cull; draw += f.draw; trans += f.trans; copy += f.copy; dyn += f.dyncull; ovl += f.ovl; recv += f.receiver;
                }
                const double n = (double)std::max<size_t>(rr.frames.size(), 1);
                double mean = 0; for (double v : tot) mean += v; mean /= n;
                A.run_mean.push_back(mean); A.run_p50.push_back(pct(tot, 0.5)); A.run_p95.push_back(pct(tot, 0.95));
                A.ref_p50.push_back(pct(ref, 0.5));
                A.cull += cull / n; A.draw += draw / n; A.trans += trans / n; A.copy += copy / n; A.dyncull += dyn / n; A.ovl += ovl / n; A.recv += recv / n;
                ++A.nruns;
                const double secs = rr.frames.size() / opt.fps;
                for (uint32_t c = 0; c < VKSC_CASCADES; ++c)
                {
                    A.renders_s[c] = rr.renders[c] / secs;
                    A.draws[c] = rr.draw_samples[c] ? (double)rr.draws[c] / rr.draw_samples[c] : 0.0;
                }
                // image diff vs the first policy (the baseline), first rep's images
                if (pi == 0 && r == 0) base_samples = std::move(rr.samples);
                else if (!base_samples.empty() && !rr.samples.empty())
                {
                    const size_t ns = std::min(base_samples.size(), rr.samples.size());
                    double dmax = 0, dsum = 0, tmax = 0; uint64_t any = 0, big = 0, cnt = 0;
                    size_t worst = 0; uint64_t worst_n = 0;
                    for (size_t s = 0; s < ns; ++s)
                    {
                        uint64_t here = 0;
                        const std::vector<uint16_t>& a = base_samples[s];
                        const std::vector<uint16_t>& b = rr.samples[s];
                        for (size_t i = 0; i < a.size(); i += 4)
                        {
                            const double dv = std::fabs((double)a[i + 3] - (double)b[i + 3]) / 65535.0;
                            double dt = 0;
                            for (int k = 0; k < 3; ++k) dt = std::max(dt, std::fabs((double)a[i + k] - (double)b[i + k]) / 65535.0);
                            dmax = std::max(dmax, dv); tmax = std::max(tmax, dt); dsum += dv; ++cnt;
                            if (std::max(dv, dt) > 1.0 / 255.0) { ++any; ++here; }
                            if (std::max(dv, dt) > 0.1) ++big;
                        }
                        if (here > worst_n) { worst_n = here; worst = s; }
                    }
                    if (!dump.empty() && r == 0)
                    {
                        // the sample with the most differing pixels: baseline vis, this policy's vis, |diff| x 8
                        const std::vector<uint16_t>& a = base_samples[worst];
                        const std::vector<uint16_t>& b = rr.samples[worst];
                        const std::string stem = dump + "/" + sc->name + "_" + run_pols[pi].name + "_s" + std::to_string(worst);
                        auto pgm = [&](const std::string& path, auto val) {
                            FILE* fp = std::fopen(path.c_str(), "wb");
                            if (!fp) return;
                            std::fprintf(fp, "P5\n%u %u\n255\n", opt.recv_w, opt.recv_h);
                            for (size_t i = 0; i < a.size(); i += 4) { const uint8_t v = val(i); std::fwrite(&v, 1, 1, fp); }
                            std::fclose(fp);
                        };
                        pgm(stem + "_base.pgm", [&](size_t i) { return (uint8_t)(a[i + 3] >> 8); });
                        pgm(stem + "_pol.pgm", [&](size_t i) { return (uint8_t)(b[i + 3] >> 8); });
                        pgm(stem + "_diff.pgm", [&](size_t i) {
                            const double d = std::fabs((double)a[i + 3] - (double)b[i + 3]) / 65535.0;
                            return (uint8_t)std::min(255.0, d * 8.0 * 255.0); });
                    }
                    A.diffed = true;
                    A.dmax = std::max(A.dmax, dmax); A.tmax = std::max(A.tmax, tmax);
                    A.dmean = std::max(A.dmean, cnt ? dsum / cnt : 0.0);
                    A.p_any = std::max(A.p_any, cnt ? 100.0 * any / cnt : 0.0);
                    A.p_big = std::max(A.p_big, cnt ? 100.0 * big / cnt : 0.0);
                }
                std::printf("[sb] %s / %s rep %d: shadow mean %.3f ms (p50 %.3f p95 %.3f), ref %.3f, renders/s %.1f %.1f %.1f %.1f\n",
                            sc->name, run_pols[pi].name.c_str(), r, mean, pct(tot, 0.5), pct(tot, 0.95), pct(ref, 0.5),
                            A.renders_s[0], A.renders_s[1], A.renders_s[2], A.renders_s[3]);
                std::fflush(stdout);
            }
        std::printf("\n=== %s (%d reps; ms per frame; 'min' = lowest run mean = least contended) ===\n", sc->name, reps);
        std::printf("%-14s %7s %7s %7s %7s | %6s %6s %6s %6s %6s %6s | %6s %6s | %-23s | %-23s | diff vs %s: max  mean  %%>1/255 %%>0.1 tint\n",
                    "policy", "mean", "min", "p50", "p95", "cull", "draw", "trans", "copy", "dcull", "ovl", "recv", "ref",
                    "renders/s c0 c1 c2 c3", "draws/frame c0 c1 c2 c3", run_pols[0].name.c_str());
        for (size_t pi = 0; pi < run_pols.size(); ++pi)
        {
            const Agg& A = agg[pi];
            const double k = 1.0 / std::max<uint32_t>(A.nruns, 1);
            char diff[128] = "";
            if (A.diffed) std::snprintf(diff, sizeof(diff), "%.4f %.5f %6.3f %6.3f %.4f", A.dmax, A.dmean, A.p_any, A.p_big, A.tmax);
            std::printf("%-14s %7.3f %7.3f %7.3f %7.3f | %6.3f %6.3f %6.3f %6.3f %6.3f %6.3f | %6.3f %6.3f | %5.1f %5.1f %5.1f %5.1f | %5.0f %5.0f %5.0f %5.0f | %s\n",
                        run_pols[pi].name.c_str(), pct(A.run_mean, 0.5), *std::min_element(A.run_mean.begin(), A.run_mean.end()),
                        pct(A.run_p50, 0.5), pct(A.run_p95, 0.5), A.cull * k, A.draw * k, A.trans * k, A.copy * k, A.dyncull * k, A.ovl * k,
                        A.recv * k, pct(A.ref_p50, 0.5), A.renders_s[0], A.renders_s[1], A.renders_s[2], A.renders_s[3],
                        A.draws[0], A.draws[1], A.draws[2], A.draws[3], diff);
        }
        std::fflush(stdout);
    }
    if (opt.validate) std::printf("[sb] validation: %u errors, %u warnings\n", ctx.validationErrorCount(), ctx.validationWarnCount());
    bench.shutdown();
    return 0;
}
