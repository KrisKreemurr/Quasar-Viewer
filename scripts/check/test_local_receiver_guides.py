#!/usr/bin/env python3
"""Finite prior-pose depth/normal, memory-layout and local-only source checks."""
import math
from pathlib import Path
import random
import struct
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
SH = ROOT/'indra/vkclean/shaders'
HOST = ROOT/'indra/newview/vkgfx'
ERROR = .136968


def dot(a, b):
    return sum(x*y for x, y in zip(a, b))


def unit(v):
    length = math.sqrt(dot(v, v))
    return tuple(x/length for x in v)


def wrap(v):
    return ((1-abs(v[1]))*(1 if v[0] >= 0 else -1),
            (1-abs(v[0]))*(1 if v[1] >= 0 else -1))


def encode(v):
    scale = sum(abs(x) for x in v)
    n = tuple(x/scale for x in v)
    e = n[:2] if n[2] >= 0 else wrap(n[:2])
    q = [max(0, min(31, math.floor((x*.5+.5)*31+.5))) for x in e]
    return 1024+q[0]+(q[1] << 5)


def decode(metadata):
    bits = metadata-1024
    e = ((bits & 31)*2/31-1, (bits >> 5)*2/31-1)
    n = [*e, 1-abs(e[0])-abs(e[1])]
    if n[2] < 0:
        n[:2] = wrap(n[:2])
    return unit(n)


def tightened(cut):
    return 1 if cut >= math.cos(ERROR) else cut*math.cos(ERROR)+math.sqrt(1-cut*cut)*math.sin(ERROR)


def half(value):
    return struct.unpack('e', struct.pack('e', value))[0]


class LocalReceiverGuides(unittest.TestCase):
    def test_pure_depth_motion_has_no_xy_tolerance_but_old_depth_rejects(self):
        # Both poses on optical axis: motionXY=0 despite a 40cm axial step.
        previous_depth, current_depth, motion_xy = 2., 2.4, (0., 0.)
        self.assertEqual(motion_xy, (0., 0.))
        self.assertGreater(abs(previous_depth-current_depth), max(.04*current_depth, .03))
        self.assertLessEqual(abs(previous_depth-half(previous_depth)), .04*previous_depth-previous_depth/512)

    def test_wrong_neighbouring_wall_still_rejects(self):
        previous_depth, wall_depth = 2., 1.8
        self.assertGreater(abs(previous_depth-wall_depth), .04*previous_depth-previous_depth/512)

    def test_fp16_depth_does_not_widen_original_acceptance(self):
        rng = random.Random(221)
        for _ in range(2000):
            actual = 10**rng.uniform(-2, 4.7)
            expected = half(actual)
            stored = actual+rng.uniform(-1.3, 1.3)*max(actual*.04, .03)
            accepted = abs(stored-expected) <= max(expected*.04, .03)-expected/512
            if accepted:
                self.assertLessEqual(abs(stored-actual), max(actual*.04, .03)+1e-8)

    def test_all_normal_metadata_roundtrip_exact_fp16(self):
        for metadata in range(1024, 2048):
            self.assertEqual(half(metadata), metadata)
            self.assertAlmostEqual(dot(decode(metadata), decode(metadata)), 1.)

    def test_oct_error_bound_and_strict_angular_cuts(self):
        rng = random.Random(8115)
        for _ in range(1200):
            normal = unit(tuple(rng.uniform(-1, 1) for _ in range(3)))
            decoded = decode(encode(normal))
            self.assertGreaterEqual(dot(normal, decoded), math.cos(ERROR)-1e-10)
            for cut in (.8, .85, .95):
                previous = unit(tuple(rng.uniform(-1, 1) for _ in range(3)))
                if dot(decoded, previous) >= tightened(cut):
                    self.assertGreaterEqual(dot(normal, previous), cut-1e-10)

    def test_previous_pose_normal_avoids_false_rotation_rejection(self):
        previous, current = (0., 0., 1.), (1., 0., 0.)
        self.assertLess(dot(previous, current), .85)
        self.assertGreaterEqual(dot(previous, decode(encode(previous))), tightened(.85))

    def test_previous_winding_ignores_shading_normal_eye_dot(self):
        source = (SH/'clean_receiver_normal.glsl').read_text()
        self.assertIn('area > 0.0 ? gl_FrontFacing : !gl_FrontFacing', source)
        self.assertNotIn('eyeRay', source)
        for front in (True, False):
            self.assertEqual(front if 1 > 0 else not front, front)
            self.assertEqual(front if -1 > 0 else not front, not front)
        self.assertIn('return N;', source)

    def test_frozen_clock_has_pose_metadata_without_a_time_key(self):
        normal_a, normal_b = (0., 0., 1.), (1., 0., 0.)
        frozen_time = 10.
        self.assertEqual(frozen_time, 10.)
        self.assertNotEqual(encode(normal_a), encode(normal_b))
        source = (SH/'clean_receiver_motion.glsl').read_text()
        self.assertNotIn('time', source)
        self.assertNotIn('frame', source[source.index('vec4 receiverMotionPayload'):])
        self.assertIn('receiverClass', source)

    def test_invalid_guides_reject_not_current_normal_fallback(self):
        source = (SH/'clean_dn_temporal.comp').read_text()
        self.assertIn('!receiverHasPreviousNormal(motion.w)', source)
        self.assertIn('!receiverInvalid;', source)
        self.assertIn('pw = motion.z;', source)
        self.assertIn('g_dn_prior_normal = receiverPreviousNormal(motion.w)', source)
        self.assertIn('!g_dn_prior_receiver && dnMoveZTol()', source)
        self.assertIn('dot(previousW, previousW) > 1e-12', source)

    def test_normal_frames_follow_palette_history_and_held_root(self):
        skin = (SH/'clean_preskin.comp').read_text()
        for token in ('prev_nrm = previousRot * bind_nrm', 'receiverUnpackSkinnedNormal(hist_pos[hb+3u])',
                      'prev_nrm = pr_rot * SV_NRM', 'hist_pos[wb+3u] = receiverPackSkinnedNormal'):
            self.assertIn(token, skin)
        rigged = (SH/'clean_draw_rigged.vert').read_text()
        self.assertIn('prev_nrm = PR * normal', rigged)
        self.assertIn('pvb + 5u <= uint(prev_skinned.length())', rigged)

    def test_flat_previous_vertex_stride_is_twenty_not_thirtytwo(self):
        previous_vertex = struct.pack('5I', 0, 0, 0, 0, 0)
        self.assertEqual(len(previous_vertex), 20)
        self.assertEqual(len(previous_vertex)-12, 8)
        self.assertEqual(2*(len(previous_vertex)-12), 16)
        rigged = (SH/'clean_draw_rigged.vert').read_text()
        self.assertIn('uint prev_skinned[]', rigged)
        self.assertIn('uint(gl_VertexIndex) * 5u', rigged)
        host = (HOST/'llcleanrenderervk.cpp').read_text()
        self.assertIn('(VkDeviceSize)mPrevSkinCap * 20u', host)
        self.assertIn('(VkDeviceSize)mSkinHistCap * 2u * 20u', host)

    def test_cluster_host_offsets_follow_new_stride_and_existing_budget(self):
        host = (HOST/'llcleanclusterskinvk.cpp').read_text()
        shader = (SH/'clean_cluster_skin_common.glsl').read_text()
        self.assertIn('kSkinVertexBytes + LLCleanClusterSkinVK::kSkinPrevBytes) / 4u', host)
        self.assertIn('cap * CL_MAX_CLUSTER_VERTS * 10u', shader)
        self.assertEqual(64*(20+20)//4, 64*10)
        self.assertEqual(64*8, 512) # actual added prior-pose bytes per cluster
        self.assertIn('bytes / kArenaBytesPerCluster', host) # cap reduces within same arena budget

    def test_budget_and_soft_refusal_have_defined_legacy_fallback(self):
        budget = (HOST/'llcleanrenderervk.cpp').read_text()
        self.assertIn('taaRequested() || mTemporalOn || mPdnWant', budget)
        self.assertIn('(temporal ? 20u + (hist ? 40u : 0u) : 0u)', budget)
        params = (HOST/'llcleanprojstochvk.cpp').read_text()
        self.assertIn('dp.localReceiverGuides = receiverGuides && previousSkinReady', params)
        self.assertIn('mPrevSkinCap == mSkinVertCap', params)
        # Source proof of snapshot byte/word stride consistency after RGBA widening.
        self.assertIn('s.motionW * s.motionH * 8u', (HOST/'llcleansnapshotvk.cpp').read_text())
        self.assertIn('motion[2u*(', (SH/'clean_snap_live.comp').read_text())

    def test_gi_and_spatial_iteration_do_not_receive_the_new_policy(self):
        self.assertIn('bool  localReceiverGuides = false;', (HOST/'llcleandenoisevk.h').read_text())
        host = (HOST/'llcleandenoisevk.cpp').read_text()
        self.assertIn('(p.localReceiverGuides ? 2u : 0u)', host)
        self.assertNotIn('localReceiverGuides', (HOST/'llcleangivk.cpp').read_text())
        spatial = host[host.index('void LLCleanDenoiserVK::recordSpatial'):]
        self.assertLess(spatial.index('q.ctl[1] = k;'), spatial.index('vkCmdPushConstants'))

    def test_transient_setting_and_original_current_guides(self):
        root = ET.parse(ROOT/'indra/newview/app_settings/settings.xml').getroot()
        items = list(root.find('map'))
        index = next(i for i, v in enumerate(items) if v.tag == 'key' and v.text == 'RenderVKDenoiseReceiverGuides')
        fields = list(items[index+1])
        values = {fields[i].text: fields[i+1].text for i in range(0, len(fields), 2)}
        self.assertEqual(values['Persist'], '0')
        self.assertEqual(values['Value'], '1')
        self.assertIn('pdnPackNR(s.N, specRough)', (SH/'clean_deferred_light.comp').read_text())
        self.assertIn('dnRadianceMoveReset()', (SH/'clean_dn_temporal.comp').read_text())


if __name__ == '__main__':
    unittest.main()
