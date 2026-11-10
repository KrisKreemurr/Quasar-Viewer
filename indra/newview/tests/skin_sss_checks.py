#!/usr/bin/env python3
"""Source-bound SSS normalization checks and an ideal-plane profile fixture.
No Vulkan, viewer, SDK, or GPU required. Outputs stay in the supplied directory.
"""
import math
from pathlib import Path
import re
import subprocess
import sys

root, out = map(Path, sys.argv[1:3])
out.mkdir(parents=True, exist_ok=True)
src = (root / 'indra/vkclean/shaders/clean_deferred_light.comp').read_text()
sss = src.split('// ---- SKIN SUBSURFACE SCATTERING (17;', 1)[1].split('#elif CLEAN_DEFER_PASS == 7', 1)[0]
helper = re.search(r'vec3 sssLobeDelta\([^}]+}', sss).group()
cpp = '''#include <cassert>
#include <cmath>
struct vec3 {
 double x,y,z;
 explicit vec3(double v):x(v),y(v),z(v){}
 vec3(double a,double b,double c):x(a),y(b),z(c){}
 vec3 operator/(double d) const {return vec3(x/d,y/d,z/d);}
};
''' + helper + '''
int main() {
 for(double w : {0., 1e-40, 1e-13, 1., 16.}) {
  auto v=sssLobeDelta(vec3(0.),w);
  assert(v.x==0 && v.y==0 && v.z==0);
 }
 auto v=sssLobeDelta(vec3(2.,4.,6.),2.);
 assert(v.x==1 && v.y==2 && v.z==3);
 // Original acc=0, wsum=0 -> E=0: subtracts the entire exported diffuse.
 const double oldLit=1.+(0./1e-12-1.);
 const auto retained=sssLobeDelta(vec3(0.),0.);
 assert(oldLit==0. && 1.+retained.x==1.);
}
'''
cpp = '#include <initializer_list>\n' + cpp
(out/'checks.cpp').write_text(cpp)
subprocess.run(['c++','-std=c++20','-Wall','-Wextra','-Werror',str(out/'checks.cpp'),'-o',str(out/'checks')],check=True)
subprocess.run([str(out/'checks')],check=True)
variances = list(map(float, re.search(r'SSS_VAR\[6\].*?float\[6\]\((.*?)\)', sss).group(1).split(',')))
weights = [tuple(map(float,x.split(','))) for x in re.findall(r'vec3\(([^)]+)\)', re.search(r'SSS_W\[6\].*?vec3\[6\]\((.*?)\);',sss,re.S).group(1))]
taps = int(re.search(r'#define SSS_LOBE_TAPS (\d+)u',sss).group(1))
assert len(variances)==len(weights)==6 and taps==4
assert all(abs(sum(w[c] for w in weights)-1)<1e-6 for c in range(3))
assert 'weightedDelta / weight' in helper
assert 'imageLoad(uSssDiff, q).x) - S0' in sss
assert 'float(g_draw_push.flags >>' not in sss  # fixed spatial pattern
assert 'sssDiffuseColor(s) * delta' in sss
assert 'SSS_P[i] * exp(' in sss
assert 'float[4](0.25, 0.675, 0.915, 0.99)' in sss
assert 'float[4](0.50, 0.35, 0.13, 0.02)' in sss

def profile(scale, offset, mm_per_pixel=0.25):
    # Ideal flat skin with a unit illumination step at x=0, shadow receiver x=-offset.
    # Production inverse-CDF radii, pixel rounding, spatial rotation and weights.
    total=[0.,0.,0.]
    quantiles=[.25,.675,.915,.99]
    probabilities=[.5,.35,.13,.02]
    n=2048
    mmpp=mm_per_pixel/scale
    for phase in range(n):
        for k,(variance,w) in enumerate(zip(variances,weights)):
            if math.sqrt(variance)<.5*mmpp: continue
            lit=0
            for i in range(taps):
                u=quantiles[i]
                r=min(math.sqrt(-2*variance*math.log(1-u))/mmpp,48.)
                a=phase/n*2*math.pi+i*2.39996323+k*1.04719755
                dx=math.floor(.5+r*math.cos(a))
                lit += probabilities[i]*(dx*mm_per_pixel>=offset)
            for c in range(3): total[c]+=w[c]*lit/n
    return total
for scale in [1.,4.]:
    edge=profile(scale,.5*scale)
    assert edge[0]>edge[1]>edge[2],edge
    print(f'ideal step, scale={scale:g}, shadow offset={.5*scale:g}mm: R/G/B={edge}')
# Pixel scale is render pixels, not display pixels: mmpp=world pixel *1000/scale.
# At mmpp=.25, scale4 profile radius is128 pixels before the existing48px cap.
print('PASS: production zero-support/constant-delta normalization; per-lobe unit mass; red>green>blue edge spread; fixed24-tap cap')

# Reproduce the old common-disc quadrature's lost broad-lobe mass at its48px
# cap (profile-space .05mm/pixel): the broadest red Gaussian gets only~.32
# of its requested mass before all lobes are normalized together.
old_mass=0.
mmpp=.05
radius=min(8./mmpp,48.)
area=3.*math.pi*(radius*mmpp)**2/16.
for phase in range(256):
    for i in range(16):
        u=(i+.5)/16.
        r=radius*u*math.sqrt(u)
        a=phase/256.*2.*math.pi+i*2.39996323
        dx=math.floor(.5+r*math.cos(a))*mmpp
        dy=math.floor(.5+r*math.sin(a))*mmpp
        old_mass += math.exp(-(dx*dx+dy*dy)/(2.*variances[-1]))/(2.*math.pi*variances[-1])*area*u*u/256.
assert .30<old_mass<.34, old_mass
print(f'old broad red-lobe mass at48px cap: {old_mass:.6f}; new per-lobe mass retained=1')
