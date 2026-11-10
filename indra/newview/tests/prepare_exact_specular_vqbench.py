#!/usr/bin/env python3
"""Copy the read-only vqbench into owned output and add a glossy raw-signal case.
Never edits the supplied harness or runs Vulkan. Build/run commands are printed.
"""
from pathlib import Path
import argparse, hashlib, json, shutil
p=argparse.ArgumentParser();p.add_argument('--bench-source',type=Path,required=True);p.add_argument('--source',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
src=a.bench_source.resolve();root=a.source.resolve();out=a.out.resolve()
if out.exists():p.error('output must not exist; preserve prior evidence')
if out==src or out.is_relative_to(src):p.error('output must not modify the supplied harness')
dst=out/'indra/vqbench';shutil.copytree(src,dst,ignore=shutil.ignore_patterns('__pycache__','build*'))
(dst.parent/'vkclean').symlink_to(root/'indra/vkclean',target_is_directory=True)
original={str(f.relative_to(src)):hashlib.sha256(f.read_bytes()).hexdigest() for f in src.rglob('*') if f.is_file()}
def edit(file,old,new):
 q=dst/file;s=q.read_text()
 if old not in s:raise RuntimeError('harness contract changed: '+file+' / '+old[:60])
 q.write_text(s.replace(old,new,1))
edit('vqb_main.cpp','bool serialize = false;','bool serialize = false, specularCase = false, exactSpecular = false;')
edit('vqb_main.cpp','else if (a == "--no-gi")','''else if (a == "--specular-baseline" || a == "--specular-exact")
        { o.specularCase = true; o.exactSpecular = a == "--specular-exact"; o.scene.glossyReceivers = true; }
        else if (a == "--no-gi")''')
edit('vqb_main.cpp','b.v4("linfo.cam_fwd",','b.u4("linfo.fwd", 0u, 0u, opt.specularCase ? (opt.exactSpecular ? 10u : 2u) : 0u, 0u);\n            b.v4("linfo.cam_fwd",')
edit('vqb_scene.h','uint32_t seed = 1039;','bool glossyReceivers = false;\n    uint32_t seed = 1039;')
edit('vqb_scene.cpp','1, 0.92f, 0, -1, 0, 64);','1, c.glossyReceivers ? 0.12f : 0.92f, c.glossyReceivers ? 0.8f : 0.f, -1, 0, 64);')
# This harness has no ReGIR producer. Valid shadow records let its synthetic
# point lights exercise the same generic point-light ray/BRDF code, without
# pretending to validate the viewer's grid population/reuse machinery.
edit('vqb_main.cpp','if (scene.lights[i].proj_origin[3] > 0.5f)\n        {','if (opt.specularCase || scene.lights[i].proj_origin[3] > 0.5f)\n        {')
edit('vqb_main.cpp','if (scene.lights[i].proj_origin[3] <= 0.5f) order.push_back(i);','if (!opt.specularCase && scene.lights[i].proj_origin[3] <= 0.5f) order.push_back(i);')
edit('vqb_main.cpp','ctx.upload(scene.bLights, 0, scene.lights.data(),','for (uint32_t i = 0; i < scene.lights.size(); ++i) scene.lights[i].tex[3] = i + 1u;\n    ctx.upload(scene.bLights, 0, scene.lights.data(),')
needle='if (first) { ref = fp; first = false; }'
edit('vqb_main.cpp',needle,'''if (opt.specularCase)
            {
                const size_t plane = size_t(DW) * DH * 4u;
                size_t rawPixels = 0;
                if (fp.pdnInRaw.size() >= 4u * plane)
                    for (size_t i = 3u * plane; i < 4u * plane; i += 4u)
                        if ((fp.pdnInRaw[i] & 0x7fffu) || (fp.pdnInRaw[i+1] & 0x7fffu) || (fp.pdnInRaw[i+2] & 0x7fffu)) ++rawPixels;
                const uint64_t diffuseHash = fp.pdnInRaw.size() >= plane ? fnv(fp.pdnInRaw.data(), plane * sizeof(uint16_t)) : 0u;
                std::printf("EXACT_SPECULAR enabled=%u diffuseHash=%llx rawExactNonzero=%zu jobs=%u\\n",
                            opt.exactSpecular ? 1u : 0u, (unsigned long long)diffuseHash, rawPixels, fp.jobs);
            }
            '''+needle)
(out/'source-manifest.json').write_text(json.dumps({'harness':str(src),'source':str(root),'original_sha256':original},indent=2))
print('Prepared raw-signal fixture (not a denoiser/beauty renderer):',dst)
print('cmake -S',dst,'-B',out/'build','-G Ninja -DCMAKE_CXX_FLAGS=-Werror')
print('nice -n 19 cmake --build',out/'build','-j2')
print('Run ONLY through vqb_run.sh; compare these separately at 320x180:')
for mode in ['--specular-baseline','--specular-exact']:
 print(dst/'vqb_run.sh',out/'build/vqbench',mode,'--configs 0 --no-gi --lights 3,3 --size 320x180 --frames 8 --warmup 2 --rounds 1')
print('Expected: equal diffuseHash, nonzero exact plane only when enabled; total shadow timings include selection and extra visibility.')
