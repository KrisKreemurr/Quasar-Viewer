#!/usr/bin/env python3
"""Finite local-DI moment policy checks; not a GPU or visual-quality test."""
import math
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
SHADERS = ROOT / 'indra/vkclean/shaders'
HOST = ROOT / 'indra/newview/vkgfx'


def recover(prior_var, prior_lum, current_lum, spatial_var, energy):
    if energy <= 0 or not all(math.isfinite(v) and v >= 0 for v in
                              (prior_var, prior_lum, current_lum, spatial_var, energy)):
        return 0.0
    scale = min(energy, 122.0)
    delta = max(-244.0, min(current_lum - prior_lum, 244.0))
    sample_var = max(min(spatial_var, 60000.0), .5 * delta * delta)
    return min(.75 * min(prior_var, 60000.0) + .25 * sample_var, 4 * scale * scale)


def eligible(enabled=True, radiance=True, current_class=2, prior_guides=True,
             invalid=False, surface_match=True, reset=True, current_w=1, prior_w=1):
    return (enabled and radiance and current_class == 2 and prior_guides and not invalid
            and surface_match and reset and current_w > 0 and math.isfinite(current_w)
            and prior_w > 0 and math.isfinite(prior_w))


class SkinnedVarianceRecovery(unittest.TestCase):
    def test_four_frame_moment_decay_is_independent_of_length_one(self):
        variance = 1.0
        for i in range(1, 9):
            variance = recover(variance, 1, 1, 0, 1)
            self.assertAlmostEqual(variance, .75 ** i)

    def test_temporal_difference_survives_collapsed_spatial_variance(self):
        self.assertEqual(recover(0, 1, 0, 0, 1), .125)

    def test_light_off_retains_no_moment_or_colour(self):
        self.assertEqual(recover(100, 10, 0, 0, 0), 0)
        for w in (0, -1, math.nan, math.inf):
            self.assertFalse(eligible(current_w=w))
            self.assertFalse(eligible(prior_w=w))

    def test_invalid_and_unmatched_previous_pose_fail_closed(self):
        self.assertFalse(eligible(invalid=True))  # code3 is still CURRENT class2
        self.assertFalse(eligible(prior_guides=False))
        self.assertFalse(eligible(surface_match=False))  # virtual-only tap cannot qualify

    def test_static_gi_ab_zero_and_nonreset_leave_baseline(self):
        for args in ({'current_class': 1}, {'radiance': False}, {'enabled': False}, {'reset': False}):
            self.assertFalse(eligible(**args))
        self.assertTrue(eligible())

    def test_finite_nonnegative_fp16_cap(self):
        for energy in (1e-10, .1, 1, 100, 60000, 1e30):
            result = recover(1e30, 1e30, 0, 1e30, energy)
            self.assertTrue(math.isfinite(result))
            self.assertGreaterEqual(result, 0)
            self.assertLessEqual(result, min(60000, 4 * energy * energy))
        for value in (-1, math.nan, math.inf):
            for index in range(5):
                args = [1.0] * 5
                args[index] = value
                self.assertEqual(recover(*args), 0)

    def test_current_class_does_not_depend_on_previous_normal_validity(self):
        shader = (SHADERS/'clean_dn_temporal.comp').read_text()
        identity = shader.index('currentRiggedReceiver = receiverMotionClass(motion.w) == 2u;')
        self.assertLess(identity, shader.index('receiverHasPreviousNormal(motion.w)'))
        self.assertIn('&& g_dn_prior_receiver && !receiverInvalid && receiverHistoryMatched && resetColour', shader)

    def test_surface_snapshot_precedes_virtual_and_exact_colour_reset(self):
        shader = (SHADERS/'clean_dn_temporal.comp').read_text()
        snapshot = shader.index('const Tap receiverHistory = h;')
        virtual = shader.index('if (histOk && kv > 0.0)')
        reset = shader.index('if (dnRadianceMoveReset() && (fMv >= 0.05 || (have && h.moved > 0.0))) have = false;')
        self.assertLess(snapshot, virtual)
        self.assertLess(virtual, reset)
        self.assertLess(reset, shader.index('float lenD = have ? h.ld : 0.0;'))
        recovery = shader[shader.index('const bool recoverVariance ='):shader.index('const uint specIt =')]
        for colour in ('accD =', 'accS =', 'nD =', 'nS =', 'hD =', 'hS ='):
            self.assertNotIn(colour, recovery)
        self.assertIn('if (receiverHistory.ld >= 0.5)', recovery)
        self.assertIn('if (receiverHistory.ls >= 0.5)', recovery)

    def test_debug_mask_is_not_history_and_uses_existing_three_bit_adapter(self):
        shader = (SHADERS/'clean_dn_temporal.comp').read_text()
        self.assertLess(shader.index('pdnPackHist(pdnSane(accD)'), shader.index('outD = vec3(currentRiggedReceiver'))
        host = (HOST/'llcleanprojstochvk.cpp').read_text()
        self.assertIn('mPdnDebug = dbg <= 10u ? dbg : 0u;', host)
        self.assertIn('litDbg = mPdnDebug >= 8u ? 4u : mPdnDebug;', host)
        combine = (SHADERS/'clean_deferred_light.comp').read_text()
        self.assertIn('if (pdbg != 0u) color = deferPdnDebug(pdbg, P, color);', combine)
        debug = combine[combine.index('vec3 deferPdnDebug('):combine.index('#endif', combine.index('vec3 deferPdnDebug('))]
        self.assertIn('return d;', debug)
        atrous = (SHADERS/'clean_dn_atrous.comp').read_text()
        mask = atrous[atrous.index('if (dbg == PDN_DBG_SKIN_RECOVERY)'):]
        self.assertLess(mask.index('return;'), mask.index('dnPrefilterVar('))

    def test_temporal_policy_does_not_expand_push_or_reach_gi(self):
        host = (HOST/'llcleandenoisevk.cpp').read_text()
        self.assertIn('(p.radiance && p.localReceiverGuides && p.skinnedVarianceRecovery ? 4u : 0u)', host)
        self.assertIn('q.ctl[1] = k;', host)
        self.assertIn('bool  skinnedVarianceRecovery = false;', (HOST/'llcleandenoisevk.h').read_text())
        common = (SHADERS/'clean_dn_common.glsl').read_text()
        self.assertIn('return dnRadiance() && (dp.ctl.y & 4u) != 0u', common)

    def test_setting_is_live_nonpersistent_default_candidate(self):
        entries = list(ET.parse(ROOT/'indra/newview/app_settings/settings.xml').getroot().find('map'))
        index = next(i for i, entry in enumerate(entries) if entry.tag == 'key'
                     and entry.text == 'RenderVKDenoiseSkinnedVarianceRecovery')
        data = list(entries[index + 1])
        setting = {data[i].text: data[i + 1].text for i in range(0, len(data), 2)}
        self.assertEqual(setting['Persist'], '0')
        self.assertEqual(setting['Value'], '1')


if __name__ == '__main__':
    unittest.main()
