#!/usr/bin/env python3
"""Finite DI-only history-reset checks; no renderer or GPU execution."""
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SHADERS = ROOT / 'indra/vkclean/shaders'
HOST = ROOT / 'indra/newview/vkgfx'


def reset_history(enabled, radiance, have, moved_byte, previous_changed):
    return have and not (enabled and radiance and
                         (moved_byte / 255 >= .05 or previous_changed))


class MoverReset(unittest.TestCase):
    def test_short_cap_still_contains_old_gobo(self):
        old, current, capped_length = .8, .1, 4
        capped = old * (1 - 1 / capped_length) + current / capped_length
        self.assertAlmostEqual(capped, .625)
        have = reset_history(True, True, True, 255, False)
        fresh = current if not have else capped
        self.assertEqual(fresh, current)
        self.assertNotEqual(capped, current)

    def test_enter_keep_moving_leave_and_settle(self):
        have, prior = True, False
        for share, expected in ((128, False), (128, False), (0, False), (0, True)):
            have = reset_history(True, True, True, share, prior)
            self.assertEqual(have, expected)
            prior = share / 255 >= .05

    def test_quantized_significance_boundary(self):
        self.assertTrue(reset_history(True, True, True, 12, False))
        self.assertFalse(reset_history(True, True, True, 13, False))
        self.assertFalse(reset_history(True, True, True, 0, True))

    def test_gi_ratio_and_legacy_ab_keep_existing_history(self):
        self.assertTrue(reset_history(True, False, True, 255, True))
        self.assertTrue(reset_history(False, True, True, 255, True))
        self.assertFalse(reset_history(True, True, False, 0, False))

    def test_rejection_precedes_colour_and_variance_selection(self):
        shader = (SHADERS/'clean_dn_temporal.comp').read_text()
        reset = shader.index('if (dnRadianceMoveReset()')
        self.assertLess(shader.index('if (histOk && kv > 0.0)'), reset)
        self.assertLess(reset, shader.index('float lenD = have ? h.ld'))
        self.assertLess(reset, shader.index('if (!have) { hS = rS; hvS = spVarS; }'))
        self.assertLess(reset, shader.index('float hvD = have ? h.vd : spVarD'))
        self.assertIn('(fMv >= 0.05 || (have && h.moved > 0.0))', shader)

    def test_temporal_flag_cannot_change_spatial_iteration_or_gi(self):
        host = (HOST/'llcleandenoisevk.cpp').read_text()
        self.assertIn('(p.radiance && p.radianceMoveReset ? 1u : 0u)', host)
        spatial = host[host.index('void LLCleanDenoiserVK::recordSpatial'):]
        self.assertLess(spatial.index('q.ctl[1] = k;'), spatial.index('vkCmdPushConstants'))
        common = (SHADERS/'clean_dn_common.glsl').read_text()
        self.assertIn('return dnRadiance() && (dp.ctl.y & 1u) != 0u', common)
        self.assertIn('bool  radianceMoveReset = false;', (HOST/'llcleandenoisevk.h').read_text())

    def test_roll_projection_and_uuid_use_existing_mask_only_in_ab(self):
        host = (HOST/'llcleanrenderervk.cpp').read_text()
        self.assertIn('mix(g.proj_mat, sizeof(g.proj_mat))', host)
        self.assertIn('mix(L.tex_uuid, sizeof(L.tex_uuid))', host)
        merge = host[host.index('if (mPdnRadianceMoveReset)'):]
        self.assertIn('for (uint32_t word = 0; word < 8u; ++word)', merge)
        self.assertIn('h.rs_flick[word] |= mRegirChanged[word]', merge)
        self.assertIn('mPdnRadianceMoveReset = radiance && radianceMoveReset', host)
        self.assertIn('moveRays = 1u; moveLights = 0u;', host)
        params = (HOST/'llcleanprojstochvk.cpp').read_text()
        self.assertIn('dp.radianceMoveReset = mPdnRadianceMoveReset && dp.moveHist != 0u', params)

    def test_live_nonpersistent_ab_does_not_force_ratio(self):
        root = ET.parse(ROOT/'indra/newview/app_settings/settings.xml').getroot()
        items = list(root.find('map'))
        def setting(key):
            index = next(i for i, item in enumerate(items) if item.tag == 'key' and item.text == key)
            entries = list(items[index + 1])
            return {entries[i].text: entries[i + 1].text for i in range(0, len(entries), 2)}
        self.assertEqual(setting('RenderVKReSTIRRadianceMoveReset')['Persist'], '0')
        self.assertEqual(setting('RenderVKReSTIRRadianceMoveReset')['Value'], '1')
        self.assertEqual(setting('RenderVKReSTIRRadiance')['Value'], '1')


if __name__ == '__main__':
    unittest.main()
