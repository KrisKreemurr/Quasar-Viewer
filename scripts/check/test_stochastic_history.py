#!/usr/bin/env python3
"""Split RAW snapshot source/numeric fixtures; no compiler or viewer required."""
import math
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SH = ROOT / 'indra/vkclean/shaders'

class SplitRawSnapshot(unittest.TestCase):
    def test_cross_workgroup_raw_reads_are_schedule_independent(self):
        # Four same-light invocations around the (7,7)/(8,8) workgroup corner.
        # Each reads a neighbour before storing its own HISTORY. Enumerate every
        # legal ordering; in-place RAW decoding must fail for some schedules.
        raw = [int(v*32766 + .5) | 0x8000 for v in [0, 1, .25, .75]]
        history = [int(math.ceil(v*2047 - 1e-3)) | (1 << 11) for v in [0, 1, .25, .75]]
        decode = lambda word: (word & 0x7fff) / 32766
        expected = tuple(decode(raw[(i+1) % 4]) for i in range(4))
        schedules = bad_in_place = 0

        def visit(state, current, observed, snapshot_observed):
            nonlocal schedules, bad_in_place
            if all(s == 2 for s in state):
                schedules += 1
                bad_in_place += tuple(observed) != expected
                self.assertEqual(tuple(snapshot_observed), expected)
                return
            for i, step in enumerate(state):
                if step == 2:
                    continue
                ns, nc, no, snap = state[:], current[:], observed[:], snapshot_observed[:]
                ns[i] += 1
                if step == 0:
                    no[i] = decode(current[(i+1) % 4])
                    snap[i] = decode(raw[(i+1) % 4])
                else:
                    nc[i] = history[i]
                visit(ns, nc, no, snap)

        visit([0]*4, raw[:], [None]*4, [None]*4)
        self.assertEqual(schedules, 2520)
        self.assertGreater(bad_in_place, 0)
        self.assertAlmostEqual(decode(history[1]), .125, places=3)
        self.assertAlmostEqual(decode(history[0]), .0625, places=3)

    def test_raw_history_final_layers_and_allocation_contract(self):
        temporal = (SH/'clean_dn_temporal.comp').read_text()
        split = temporal.split('void dnSplitTemporal(', 1)[1].split('void main()', 1)[0]
        self.assertEqual(split.count('imageLoad(uDnGeom, ivec3(m, PDN_SPLIT_GEOM_FINAL))'), 1)
        self.assertEqual(split.count('imageLoad(uDnGeom, ivec3(q, PDN_SPLIT_GEOM_FINAL))'), 1)
        self.assertNotIn('imageLoad(uDnGeom, ivec3(q, gl))', split)
        self.assertIn('imageLoad(uDnGeom, ivec3(q, PDN_SPLIT_GEOM_PAR + int(pl)))', split)
        self.assertNotIn('imageStore(uDnGeom, ivec3(m, PDN_SPLIT_GEOM_FINAL)', split)
        self.assertIn('imageStore(uDnGeom, ivec3(m, gl)', split)
        host = (ROOT/'indra/newview/vkgfx/llcleandenoisevk.cpp').read_text()
        self.assertIn('kDnSplitGeomPar = 2u, kDnSplitGeomFinal = 4u;', host)
        self.assertIn('index == 2u && split ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0u', host)
        self.assertIn('e.f, dnUsage(i, split), false)', host) # matching budget query
        self.assertIn('const VkImageUsageFlags usage = dnUsage(i, split);', host)
        for par in [0, 1]:
            layers = {'raw_source': 2+par, 'old_history': 2+(par ^ 1), 'snapshot': 4}
            self.assertEqual(len(set(layers.values())), 3)
            self.assertTrue(all(0 <= layer < 5 for layer in layers.values()))
        # Split=false (GI and placeholders) keeps the original usage and no copy.
        self.assertIn('if (mSplit) dnSplitRawSnapshot(cmd, mGeom.image, mW, mH, mParity);', host)
        gi = (ROOT/'indra/newview/vkgfx/llcleangivk.cpp').read_text()
        self.assertIn('mGiDn->ensure(w, h, false)', gi)

    def test_copy_dependencies_and_final_consumer_order(self):
        host = (ROOT/'indra/newview/vkgfx/llcleandenoisevk.cpp').read_text()
        copy = host.split('static void dnSplitRawSnapshot(', 1)[1].split('void LLCleanDenoiserVK::recordTemporal(', 1)[0]
        pre, rest = copy.split('vkCmdCopyImage(', 1)
        post = rest.split(';', 1)[1]
        for contract in ['VK_IMAGE_LAYOUT_GENERAL', 'VK_QUEUE_FAMILY_IGNORED',
                         'VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT',
                         'VK_ACCESS_2_SHADER_STORAGE_READ_BIT', 'VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT',
                         'VK_PIPELINE_STAGE_2_COPY_BIT', 'VK_ACCESS_2_TRANSFER_READ_BIT',
                         'VK_ACCESS_2_TRANSFER_WRITE_BIT']:
            self.assertIn(contract, pre)
        self.assertIn('b[0].srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;', post)
        self.assertIn('b[0].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;', post)
        self.assertIn('b[1].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;', post)
        self.assertIn('b[1].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;', post)
        self.assertEqual(copy.count('vkCmdPipelineBarrier2(cmd, &d);'), 2)
        self.assertIn('region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kDnSplitGeomPar + par, 1};', copy)
        self.assertIn('region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, kDnSplitGeomFinal, 1};', copy)
        self.assertIn('region.extent = {w, h, 1};', copy)
        temporal = host.split('void LLCleanDenoiserVK::recordTemporal(', 1)[1].split('void LLCleanDenoiserVK::recordSpatial(', 1)[0]
        self.assertLess(temporal.index('dnComputeBarrier(cmd)'), temporal.index('dnSplitRawSnapshot(cmd'))
        self.assertLess(temporal.index('dnSplitRawSnapshot(cmd'), temporal.index('vkCmdDispatch(cmd'))
        spatial = host.split('void LLCleanDenoiserVK::recordSpatial(', 1)[1]
        self.assertLess(spatial.index('dnComputeBarrier(cmd)'), spatial.index('vkCmdDispatch(cmd'))
        shader = (SH/'clean_dn_atrous.comp').read_text()
        self.assertIn('if (last) imageStore(uDnGeom, ivec3(m, PDN_SPLIT_GEOM_FINAL)', shader)
        self.assertIn('if (last)\n        imageStore(uDnGeom, ivec3(m, PDN_SPLIT_GEOM_FINAL)', shader)
        renderer = (ROOT/'indra/newview/vkgfx/llcleanrenderervk.cpp').read_text()
        self.assertIn('if (g.waitGfx) waits.push_back(semInfo(mTlGfx, g.waitGfx, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT))', renderer)
        self.assertIn('mAsyncGfxOpen.waitComp = mAsyncJoinedLast;', renderer)
        self.assertIn('mAsyncGfxOpen.waitCompStage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;', renderer)

    def test_raw_bypass_does_not_use_neighbour_mean(self):
        source = (SH/'clean_dn_temporal.comp').read_text().split('void dnSplitTemporal(', 1)[1].split('void main()', 1)[0]
        raw = source.split('if (rawV)', 1)[1].split('else', 1)[0]
        self.assertIn('val = r >= 0.0 ? r : 1.0;', raw)
        self.assertIn('len = 0.0;', raw)
        for unused in ['nn[', 's1[', 's2[', 'hv[', 'hl[']:
            self.assertNotIn(unused, raw)
        # This source contract limits attribution: the RAW/RR branch does not
        # use the neighbour mean, even though the immutable read is now safe.


def exact_hc(q, r, hc):
    b = math.floor(hc)
    c0, c1 = math.floor((b + .5) * r) + .5, math.floor((b + 1.5) * r) + .5
    if q < c0:
        b -= 1
        c1, c0 = c0, math.floor((b + .5) * r) + .5
    elif q >= c1:
        b += 1
        c0, c1 = c1, math.floor((b + 1.5) * r) + .5
    return b + max(0, min(1, (q - c0) / max(c1 - c0, 1e-3)))

class HalfHistory(unittest.TestCase):
    def test_static_sample_centres(self):
        for render, denoise in [(1920,1920), (1920,960), (1919,959), (1081,540)]:
            r = render / denoise
            for m in range(denoise):
                q = math.floor((m + .5) * r) + .5
                self.assertAlmostEqual(exact_hc(q, r, q / r - .5), m)
        self.assertEqual(1.5 / 2 - .5, .25)  # baseline half-grid drift

    def test_translating_sample(self):
        for r in [1, 2, 1919 / 959]:
            for m in range(2, 20):
                q0 = math.floor((m + .5) * r) + .5
                q1 = math.floor((m + 1.5) * r) + .5
                q = q0 + .3 * (q1 - q0)
                self.assertAlmostEqual(exact_hc(q,r,q/r-.5),m+.3)

    def test_radiance_budget_and_feedback(self):
        host = (ROOT/'indra/newview/vkgfx/llcleanrenderervk.cpp').read_text()
        self.assertIn('if (mPdnRadiance) { moveRays = 1u; moveLights = 0u; }',host)
        self.assertIn('k == 0u && !pdnMetaConv(g.meta)', (SH/'clean_dn_atrous.comp').read_text())
        self.assertIn('h.moved > 0.0 && fMv < 0.05', (SH/'clean_dn_temporal.comp').read_text())


if __name__ == '__main__':
    unittest.main()
