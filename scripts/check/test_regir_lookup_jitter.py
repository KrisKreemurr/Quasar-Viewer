#!/usr/bin/env python3
"""Finite proposal-support and estimator checks for primary DI lookup jitter."""
import math
import random
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SHADERS = ROOT / 'indra/vkclean/shaders'


def dot(a, b):
    return sum(x*y for x, y in zip(a, b))


def cross(a, b):
    return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])


def unit(a):
    length = math.sqrt(dot(a, a))
    return tuple(x/length for x in a)


def offset(normal, u, v, edge):
    n = unit(normal)
    t = unit(cross((0, 0, 1) if abs(n[2]) < .999 else (0, 1, 0), n))
    b = cross(n, t)
    radius = .5*edge*math.sqrt(u)
    angle = 2*math.pi*v
    return tuple(radius*(math.cos(angle)*x+math.sin(angle)*y) for x, y in zip(t, b))


class LookupJitter(unittest.TestCase):
    def test_receiver_plane_radius_and_expanded_cell_support(self):
        # Oblique normals, all clipmap scales, boundaries including clamped edges:
        # the ORIGINAL receiver must remain in every selected cell's support box.
        rng = random.Random(1317)
        for _ in range(5000):
            edge = rng.choice((1, 2, 4, 8))
            normal = unit(tuple(rng.uniform(-1, 1) for _ in range(3)))
            p = tuple(rng.uniform(0, n*edge) for n in (32, 32, 16))
            delta = offset(normal, rng.random(), rng.random(), edge)
            self.assertAlmostEqual(dot(normal, delta), 0, places=11)
            self.assertLessEqual(math.sqrt(dot(delta, delta)), .5*edge+1e-10)
            for axis, size in enumerate((32, 32, 16)):
                cell = max(0, min(size-1, math.floor((p[axis]+delta[axis])/edge)))
                self.assertGreaterEqual(p[axis], (cell-.5)*edge-1e-10)
                self.assertLessEqual(p[axis], (cell+1.5)*edge+1e-10)

    def test_support_expansion_is_required_for_short_range_sources(self):
        # Receiver0.99, selected neighbouring cell[1,2], source0.98/range0.02.
        # Unexpanded neighbour wrongly excludes this actual receiver's source.
        p, light, radius = .99, .98, .02
        self.assertLess(abs(p-light), radius)
        self.assertGreaterEqual(abs(1-light), radius)
        self.assertLess(abs(max(.5, min(2.5, light))-light), radius)

    def test_selected_cell_conditional_inverse_q_not_other_cell_q(self):
        # Each selected conditional proposal estimates the SAME actual-P target.
        # Jitter mixes proposals, not target evaluation positions or q denominators.
        target = (2.0, 9.0, 4.0)
        cells = ((.8, .1, .1), (.1, .2, .7))
        for proposal in cells:
            estimate = sum(q*(t/q) for q, t in zip(proposal, target))
            self.assertAlmostEqual(estimate, sum(target))
        wrong = sum(q*(t/other) for q, other, t in zip(cells[0], cells[1], target))
        self.assertNotAlmostEqual(wrong, sum(target))

    def test_shading_targets_and_emitter_population_are_not_jittered(self):
        source = (SHADERS/'clean_deferred_light.comp').read_text()
        self.assertIn('rsCell = rsPrimaryLookupCell(P, Ng, rp, frame)', source)
        self.assertIn('const uint cw = rsCellWord(rsCell)', source)
        self.assertIn('pdnTargetGoboL(li, P, s.N, true, p0)', source)
        self.assertIn('p * uintBitsToFloat(sl.y) * scale', source)
        self.assertIn('const uint emitterCell = emOn ? regirCell(P)', source)
        self.assertIn('ec == 0u ? emitterCell : ovCell', source)
        # This release has neither the later fixed-DI route nor layered stereo.
        # The port must not pull either implementation in as a dependency.
        self.assertFalse((SHADERS/'clean_fixed_light_terms.glsl').exists())

    def test_builder_pop0_and_disabled_mode_are_unchanged(self):
        build = (SHADERS/'clean_regir_build.comp').read_text()
        lookup = (SHADERS/'clean_regir_di_lookup.glsl').read_text()
        self.assertIn('pop == 1u && (rsf & RS_F_LOOKUP_JITTER) != 0u', build)
        self.assertIn('(rg.rs_cfg.z & RS_F_LOOKUP_JITTER) == 0u) return base', lookup)
        self.assertIn('const uint base = regirCell(P)', lookup)
        self.assertIn('const uint level = base / REGIR_CELLS_PER_LEVEL', lookup)
        self.assertNotIn('regirCell(lookup)', lookup)
        self.assertNotIn('rayQuery', lookup)

    def test_transient_default_and_primary_rt_route_guard(self):
        root = ET.parse(ROOT/'indra/newview/app_settings/settings.xml').getroot()
        elements = list(root.find('map'))
        index = next(i for i, item in enumerate(elements) if item.tag == 'key' and item.text == 'RenderVKReSTIRLookupJitter')
        entries = list(elements[index+1])
        values = {entries[i].text: entries[i+1].text for i in range(0, len(entries), 2)}
        self.assertEqual(values['Persist'], '0')
        self.assertEqual(values['Value'], '1')
        host = (ROOT/'indra/newview/vkgfx/llcleanrenderervk.cpp').read_text()
        self.assertIn('jitter && mRsNow && !mOffscreen', host)
        start = host.index('// Transient Bug2 A/B:')
        end = host.index('// (RS_F_WCUT:', start)
        self.assertNotIn('stereoActive', host[start:end])
        self.assertNotIn('mFixedDi', host[start:end])

    def test_existing_noise_pixel_and_frame_decorrelation(self):
        # Existing IGN/R2 is deterministic and spatially varying; no spectral
        # claim of a new blue-noise texture is made by these finite checks.
        def fract(x): return x-math.floor(x)
        def sample(x, y, frame):
            return fract(52.9829189*fract((x+.5)*.06711056+(y+.5)*.00583715)+fract(frame*.7548776662))
        values = [sample(x, 17, 31) for x in range(32)]
        self.assertGreater(len(set(values)), 30)
        self.assertEqual(sample(23, 17, 31), sample(23, 17, 31))
        self.assertNotEqual(sample(23, 17, 31), sample(23, 17, 32))


if __name__ == '__main__':
    unittest.main()
