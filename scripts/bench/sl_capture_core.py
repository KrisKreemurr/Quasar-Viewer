#!/usr/bin/env python3
# Copyright (C) 2026, Slipstream contributors.
# This library is free software; you can redistribute it and/or modify it under the terms of
# the GNU Lesser General Public License as published by the Free Software Foundation;
# version 2.1 of the License only.
# (A copy of render2/blender/sl_capture_core.py from the render2 branch, which main does not carry yet:
# scripts/bench/sealed_room.py builds its synthetic captures with it. Keep the two in step; when render2
# lands, sealed_room.py imports that one and this copy goes.)
"""Bench capture writer core: plain Python data in, a capture directory out (no bpy).

The capture format is the one doc/bench.md describes and indra/newview/slbenchscene.h
defines (format version 1, or 2 when the scene holds skins); the viewer's --bench replay (slbenchreplay.cpp) and render2's
slrenderlab both read it. export_capture.py turns a Blender scene into the dict below and
calls write_capture(); tests build the dict by hand.

SCENE DICT (units: metres, Z up, right handed; colours linear; matrices row-major 4x4
nested lists or numpy arrays, column vectors, i.e. Blender's Matrix layout):

  name          str, goes into the manifest ("region")
  seconds       float, track length (lights / movers); default 1 / track_hz
  track_hz      float, default 60 (the viewer's own captures use 30; the replay honours it)
  textures      [ {name, width, height, channels (3|4), pixels: bytes, rows top to bottom} ]
  materials     [ {name, base_color [r,g,b,a], metallic, roughness, emissive [r,g,b],
                   emissive_strength, alpha_mode "OPAQUE"|"MASK"|"BLEND", alpha_cutoff,
                   glow, fullbright (bool),
                   textures {base, normal, mr, emissive: texture name}} ]
  meshes        { mesh_key: [ face ] }, face = {material: index into materials,
                   positions Nx3, normals Nx3, uvs Nx2 (or None), tangents Nx4 (or None),
                   indices (3*T,)}; a face over 65535 vertices is split into several slots
  objects       [ {name, mesh: mesh_key, matrix 4x4 (object to world, scale included),
                   no_shadow (bool), track: optional [matrix or None per track sample]
                   (None = the object is gone from that sample on),
                   skin: optional index into skins (a RIGGED object: its faces carry joints /
                   weights; the matrix is only the static fallback, the skin places it)} ]
  skins         (format version 2) [ {name, armature: index into armatures, joint_nums [armature
                   joint per skin joint, <= 110], inv_bind [4x4 per skin joint], bind_shape 4x4} ]
                   palette = world[joint_nums[j]] * inv_bind[j]; skinned = sum w * palette * bind_shape * v
                   (mesh -> agent space). A face of a rigged object also has joints Nx4 (skin joint
                   indices) and weights Nx4 (top four influences, normalised here).
  armatures     (format version 2) [ {name, joint_count (<= 216), track: [ (joint_count, 4, 4) array
                   per track sample, the joint WORLD matrices, agent space (affine; only the upper 3x4 is stored); a shorter list holds its
                   last pose] } ]
  lights        [ {name, type "point"|"spot", matrix 4x4 (pose; its scale = the SL fixture
                   prim size), color [r,g,b], intensity (0..1), radius (m), falloff,
                   fov (rad), focus, ambiance, gobo: texture name or None,
                   track: optional [dict of overrides per track sample]
                   (keys: matrix, color, intensity, radius, falloff, fov, focus, ambiance,
                   enabled)} ]
  sun           {direction (toward the sun), color, ambient, sky_horizon, sky_zenith}
  camera        {keys: [ {t, matrix (Blender camera: looks down -Z, up +Y) or pos + rot
                   (SL frame quaternion x,y,z,w), fov (vertical, rad), segment, still,
                   name} ]}
  env_from      optional path of a real capture: its env.bin / sky.xml / water.xml (and the
                textures the environment names) are used instead of the defaults
  unsupported   [ {kind, name, detail} ] -> the sidecar, and listed under manifest "missing"
  extras        dict, copied into the sidecar unchanged

Conventions carried over from the viewer's own capture producers (so both pipelines read a
synthetic capture the way they read a real one):
  * One slot = one face (U16 indices, <= 65535 vertices). One instance row per face.
  * Per-face base colour goes in the instance tint; the material's base_color_factor stays
    (1, 1, 1, 1) (the viewer's ingest does the same: applying both would square it).
  * Geometry is stored in object space; the instance model matrix places it (column-major).
    The instance AABB is object-local.
  * Material id 0 is the replay's default white: written materials start at 1.
  * Projector matrices are derived exactly like the viewer's frame builder derives them
    from an SL projector prim (llappviewer.cpp buildVulkanWorldFrame).
"""

import json
import math
import os
import shutil
import struct
import time
import uuid
import zlib

try:
    import numpy as np
except ImportError:   # pragma: no cover - Blender and the tool hosts ship numpy
    np = None

FORMAT_VERSION = 1
MAGIC = b"SLBENCH1"
KIND_INSTANCES, KIND_GROUPS, KIND_SLOTS, KIND_MATERIALS, KIND_TERRAIN, KIND_ENV, KIND_LIGHTS, KIND_MOVERS = range(1, 9)
KIND_SKINS, KIND_POSES = 9, 10          # format version 2
FORMAT_VERSION_SKINS = 2                 # a capture holding skins / poses is written as version 2
MAX_SKIN_JOINTS, MAX_ARMATURE_JOINTS, MAX_SKINS, MAX_ARMATURES = 110, 216, 4096, 256
WEIGHT_ONE = 65535
FLAG_RIGGED = 1
SENTINEL = 0xFFFFFFFF
NUM_LOD = 4
QV_HEADER = 2
MAX_SLOT_VERTS = 65535
MOVER_POSE, MOVER_GONE = 0, 1

# Instance flag bits (llcleaninstancestore.h).
FLAG_ALPHA = 1 << 1
FLAG_MASK = 1 << 2
FLAG_FULLBRIGHT = 1 << 4
FLAG_EMISSIVE_NO_SHADOW = 1 << 14

# Record sizes (checked against the C++ structs with g++ sizeof / offsetof).
SIZE = {"FileHeader": 24, "Instance": 224, "InstanceRec": 232, "GroupRec": 20, "SlotRec": 16,
        "QVertex": 20, "QWeight": 10, "MaterialRec": 220, "TerrainRec": 184, "EnvRec": 856,
        "SampleHdr": 8, "MoverRec": 104, "SkinRec": 7560, "PoseHdr": 16, "LLVKLight": 256, "LLVKProbe": 104, "LLVKProbeRegion": 1448}

# The viewer's MAX_VK_LIGHTS: a sample with more lights is written, and flagged.
VIEWER_MAX_LIGHTS = 256
UUID_NAMESPACE = uuid.UUID("5be7c400-1304-4e7c-9a2b-72656e646572")

VIEWER_MISSING = [
    "avatars (rigged rows, skins, joint and pose tracks)", "HUD and worn rigid attachments",
    "particles", "objects that appear after the capture starts",
    "environment changes during the capture (the first frame's sky and water are kept)",
    "light-takeover face links",
]


class CaptureError(Exception):
    pass


# ---------------------------------------------------------------------------------------
# XXH3-64 (seed 0, default secret): the manifest checksum and capture id. The viewer's
# HBXXH64 is XXH3_64bits (indra/llcommon/hbxxh.cpp), not classic XXH64, although the
# manifest field is called "xxh64". Uses the xxhash module when present.

_M64 = (1 << 64) - 1
_P32_1, _P32_2, _P32_3 = 0x9E3779B1, 0x85EBCA77, 0xC2B2AE3D
_P64_1, _P64_2, _P64_3, _P64_4, _P64_5 = (0x9E3779B185EBCA87, 0xC2B2AE3D27D4EB4F, 0x165667B19E3779F9,
                                          0x85EBCA77C2B2AE63, 0x27D4EB2F165667C5)
_PMX1, _PMX2 = 0x165667919E3779F9, 0x9FB21C651E98DF25
_SECRET = bytes.fromhex(
    "b8fe6c3923a44bbe7c01812cf721ad1cded46de9839097db7240a4a4b7b3671f"
    "cb79e64eccc0e578825ad07dccff7221b8084674f743248ee03590e6813a264c"
    "3c2852bb91c300cb88d0658b1b532ea371644897a20df94e3819ef46a9deacd8"
    "a8fa763fe39c343ff9dcbbc7c70b4f1d8a51e04bcdb45931c89f7ec9d9787364"
    "eac5ac8334d3ebc3c581a0fffa1363eb170ddd51b7f0da49d316552629d4689e"
    "2b16be587d47a1fc8ff8b8d17ad031ce45cb3a8f95160428afd7fbcabb4b407e")
_STRIPE, _ACC_NB, _BLOCK_STRIPES = 64, 8, (len(_SECRET) - 64) // 8   # 16 stripes, 1024 B blocks


def _r64(b, o):
    return int.from_bytes(b[o:o + 8], "little")


def _r32(b, o):
    return int.from_bytes(b[o:o + 4], "little")


def _rotl64(x, r):
    return ((x << r) | (x >> (64 - r))) & _M64


def _fold(a, b):
    p = a * b
    return (p & _M64) ^ (p >> 64)


def _xxh64_aval(h):
    h ^= h >> 33
    h = (h * _P64_2) & _M64
    h ^= h >> 29
    h = (h * _P64_3) & _M64
    return h ^ (h >> 32)


def _xxh3_aval(h):
    h ^= h >> 37
    h = (h * _PMX1) & _M64
    return h ^ (h >> 32)


def _rrmxmx(h, n):
    h ^= _rotl64(h, 49) ^ _rotl64(h, 24)
    h = (h * _PMX2) & _M64
    h ^= (h >> 35) + n
    h = (h * _PMX2) & _M64
    return h ^ (h >> 28)


def _mix16(d, o, so):
    return _fold(_r64(d, o) ^ _r64(_SECRET, so), _r64(d, o + 8) ^ _r64(_SECRET, so + 8))


def _accumulate_py(acc, d, o, so):
    for i in range(8):
        v = _r64(d, o + 8 * i)
        k = v ^ _r64(_SECRET, so + 8 * i)
        acc[i ^ 1] = (acc[i ^ 1] + v) & _M64
        acc[i] = (acc[i] + (k & 0xFFFFFFFF) * (k >> 32)) & _M64


def _scramble(acc):
    for i in range(8):
        a = acc[i]
        a ^= a >> 47
        a ^= _r64(_SECRET, len(_SECRET) - 64 + 8 * i)
        acc[i] = (a * _P32_1) & _M64


def _long(d):
    n = len(d)
    acc = [_P32_3, _P64_1, _P64_2, _P64_3, _P64_4, _P32_2, _P64_5, _P32_1]
    block = _STRIPE * _BLOCK_STRIPES
    nb = (n - 1) // block
    if np is not None and nb:
        # The 16 stripes of a block only add into the accumulators (wrapping uint64 sums), so
        # a block is a few numpy reductions; the scramble between blocks stays scalar.
        keys = np.stack([np.frombuffer(_SECRET[8 * s:8 * s + 64], dtype="<u8") for s in range(_BLOCK_STRIPES)])
        swap = [1, 0, 3, 2, 5, 4, 7, 6]
        mask = np.uint64(0xFFFFFFFF)
        sh = np.uint64(32)
        for b in range(nb):
            v = np.frombuffer(d, dtype="<u8", count=_BLOCK_STRIPES * 8, offset=b * block).reshape(_BLOCK_STRIPES, 8)
            k = v ^ keys
            prod = ((k & mask) * (k >> sh)).sum(axis=0, dtype=np.uint64)
            vs = v.sum(axis=0, dtype=np.uint64)[swap]
            for i in range(8):
                acc[i] = (acc[i] + int(prod[i]) + int(vs[i])) & _M64
            _scramble(acc)
    else:
        for b in range(nb):
            for s in range(_BLOCK_STRIPES):
                _accumulate_py(acc, d, b * block + s * _STRIPE, s * 8)
            _scramble(acc)
    stripes = ((n - 1) - block * nb) // _STRIPE
    for s in range(stripes):
        _accumulate_py(acc, d, nb * block + s * _STRIPE, s * 8)
    _accumulate_py(acc, d, n - _STRIPE, len(_SECRET) - _STRIPE - 7)
    r = (n * _P64_1) & _M64
    for i in range(4):
        r = (r + _fold(acc[2 * i] ^ _r64(_SECRET, 11 + 16 * i), acc[2 * i + 1] ^ _r64(_SECRET, 11 + 16 * i + 8))) & _M64
    return _xxh3_aval(r)


def xxh3_64_pure(data):
    """XXH3-64 (seed 0) in Python (numpy speeds up inputs over 1 KiB); the fallback when the
    xxhash module is missing."""
    d = bytes(data)
    n = len(d)
    if n == 0:
        return _xxh64_aval(_r64(_SECRET, 56) ^ _r64(_SECRET, 64))
    if n <= 3:
        c = (d[0] << 16) | (d[n >> 1] << 24) | d[n - 1] | (n << 8)
        return _xxh64_aval(c ^ (_r32(_SECRET, 0) ^ _r32(_SECRET, 4)))
    if n <= 8:
        v = (_r32(d, n - 4) + (_r32(d, 0) << 32)) & _M64
        return _rrmxmx(v ^ (_r64(_SECRET, 8) ^ _r64(_SECRET, 16)), n)
    if n <= 16:
        lo = _r64(d, 0) ^ (_r64(_SECRET, 24) ^ _r64(_SECRET, 32))
        hi = _r64(d, n - 8) ^ (_r64(_SECRET, 40) ^ _r64(_SECRET, 48))
        swapped = int.from_bytes(lo.to_bytes(8, "little"), "big")
        return _xxh3_aval((n + swapped + hi + _fold(lo, hi)) & _M64)
    if n <= 128:
        acc = (n * _P64_1) & _M64
        if n > 32:
            if n > 64:
                if n > 96:
                    acc += _mix16(d, 48, 96) + _mix16(d, n - 64, 112)
                acc += _mix16(d, 32, 64) + _mix16(d, n - 48, 80)
            acc += _mix16(d, 16, 32) + _mix16(d, n - 32, 48)
        acc += _mix16(d, 0, 0) + _mix16(d, n - 16, 16)
        return _xxh3_aval(acc & _M64)
    if n <= 240:
        acc = (n * _P64_1) & _M64
        for i in range(8):
            acc += _mix16(d, 16 * i, 16 * i)
        acc = _xxh3_aval(acc & _M64)
        for i in range(8, n // 16):
            acc += _mix16(d, 16 * i, 16 * (i - 8) + 3)
        acc += _mix16(d, n - 16, 136 - 17)
        return _xxh3_aval(acc & _M64)
    return _long(d)


try:
    import xxhash as _xxhash

    def xxh3_64(data):
        return _xxhash.xxh3_64_intdigest(bytes(data))
except ImportError:   # pragma: no cover - depends on the host
    _xxhash = None

    def xxh3_64(data):
        return xxh3_64_pure(data)


def hex64(v):
    return "%016x" % v


def file_digest(path):
    with open(path, "rb") as f:
        return hex64(xxh3_64(f.read()))


# ---------------------------------------------------------------------------------------
# PNG (8-bit RGB / RGBA, rows top to bottom; what LLImagePNG reads back)

def png_bytes(width, height, channels, pixels):
    if channels not in (1, 3, 4):
        raise CaptureError("PNG: %d channels unsupported" % channels)
    stride = width * channels
    if len(pixels) != stride * height:
        raise CaptureError("PNG: pixel buffer is %d bytes, expected %d" % (len(pixels), stride * height))
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        raw += pixels[y * stride:(y + 1) * stride]
    ctype = {1: 0, 3: 2, 4: 6}[channels]

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, ctype, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))


# ---------------------------------------------------------------------------------------
# Quantized vertex (llcleanqvertex.h): 20 B per vertex, a 40 B decode header per slot.

def axis_lattice(lo, hi):
    if not (math.isfinite(lo) and math.isfinite(hi)) or lo > hi:
        lo = hi = 0.0
    half, mid = 0.5 * (hi - lo), 0.5 * (hi + lo)
    amax = max(abs(lo), abs(hi))
    e = -24
    if half > 0.0:
        e = max(e, math.ceil(math.log2(half / 32766.0)))
    if amax > 0.0:
        e = max(e, math.ceil(math.log2(amax)) - 23)
    for _ in range(160):
        sd = math.ldexp(1.0, e)
        cd = round(mid / sd) * sd   # round(): half to even, like nearbyint
        if cd - lo <= 32767.0 * sd and hi - cd <= 32767.0 * sd:
            return float(np.float32(cd)), float(np.float32(sd))
        e += 1
    return 0.0, 1.0


def make_header(positions, uvs):
    """QHeader (pos_c[3], pos_s[3], uv_c[2], uv_s[2]) for float32 arrays."""
    pc, ps, uc, us = [], [], [], []
    p = positions[np.all(np.isfinite(positions), axis=1)] if len(positions) else positions
    for k in range(3):
        lo, hi = (float(p[:, k].min()), float(p[:, k].max())) if len(p) else (0.0, 0.0)
        c, s = axis_lattice(lo, hi)
        pc.append(c)
        ps.append(s)
    t = uvs[np.all(np.isfinite(uvs), axis=1)] if uvs is not None and len(uvs) else None
    for k in range(2):
        lo, hi = (float(t[:, k].min()), float(t[:, k].max())) if t is not None and len(t) else (0.0, 0.0)
        c, s = axis_lattice(lo, hi)
        uc.append(c)
        us.append(s)
    return pc, ps, uc, us


def _lattice_index(v, c, s):
    v = np.where(np.isfinite(v), v.astype(np.float64), c)
    q = np.rint((v - np.float64(c)) / np.float64(s))
    return np.clip(q, -32767, 32767).astype(np.int16)


def _snorm16(v):
    v = np.where(np.isfinite(v), v, np.float32(0)).astype(np.float32)
    return np.rint(np.clip(v, np.float32(-1), np.float32(1)) * np.float32(32767)).astype(np.int16)


def oct_encode(d):
    d = np.asarray(d, dtype=np.float32)
    l1 = np.abs(d).sum(axis=1)
    ok = np.isfinite(l1) & (l1 > np.float32(1e-30))
    safe = np.where(ok, l1, np.float32(1))[:, None]
    n = d / safe
    x, y, z = n[:, 0], n[:, 1], n[:, 2]
    sx = np.where(x >= 0, np.float32(1), np.float32(-1))
    sy = np.where(y >= 0, np.float32(1), np.float32(-1))
    ox = (np.float32(1) - np.abs(y)) * sx
    oy = (np.float32(1) - np.abs(x)) * sy
    x2 = np.where(z < 0, ox, x)
    y2 = np.where(z < 0, oy, y)
    out = np.stack([_snorm16(x2), _snorm16(y2)], axis=1)
    out[~ok] = 0
    return out


def oct_decode(q):
    q = np.asarray(q, dtype=np.float32)
    x = np.maximum(np.float32(-1), q[:, 0] / np.float32(32767))
    y = np.maximum(np.float32(-1), q[:, 1] / np.float32(32767))
    z = np.float32(1) - np.abs(x) - np.abs(y)
    t = np.maximum(-z, np.float32(0))
    x = x + np.where(x >= 0, -t, t)
    y = y + np.where(y >= 0, -t, t)
    v = np.stack([x, y, z], axis=1)
    ln = np.linalg.norm(v, axis=1, keepdims=True)
    return np.where(ln > 0, v / np.where(ln > 0, ln, 1), np.array([0, 0, 1], np.float32))


def quantize_face(positions, normals, uvs, tangents):
    """Header-prefixed QVertex range as bytes, plus the header."""
    positions = np.asarray(positions, dtype=np.float32).reshape(-1, 3)
    n = len(positions)
    normals = np.asarray(normals, dtype=np.float32).reshape(-1, 3) if normals is not None else np.tile(
        np.array([0, 0, 1], np.float32), (n, 1))
    uvs = np.asarray(uvs, dtype=np.float32).reshape(-1, 2) if uvs is not None else np.zeros((n, 2), np.float32)
    hdr = make_header(positions, uvs)
    pc, ps, uc, us = hdr
    rec = np.zeros((n, 10), dtype=np.int16)
    for k in range(3):
        rec[:, k] = _lattice_index(positions[:, k], pc[k], ps[k])
    rec[:, 4] = _lattice_index(uvs[:, 0], uc[0], us[0])
    rec[:, 5] = _lattice_index(uvs[:, 1], uc[1], us[1])
    rec[:, 6:8] = oct_encode(normals)
    if tangents is not None:
        tangents = np.asarray(tangents, dtype=np.float32).reshape(-1, 4)
        t2 = (tangents[:, :3] ** 2).sum(axis=1)
        has = np.isfinite(t2) & (t2 > np.float32(1e-24))
        flags = np.where(has, 1, 0) | np.where(has & (tangents[:, 3] < 0), 2, 0)
        rec[:, 3] = flags.astype(np.int16)
        enc = oct_encode(tangents[:, :3])
        enc[~has] = 0
        rec[:, 8:10] = enc
    header = struct.pack("<10f", *(pc + ps + uc + us))
    return header + rec.astype("<i2").tobytes(), hdr


def decode_face(range_bytes, nv):
    """(positions, uvs, normals, tangents) of a header-prefixed range (the replay's decodeVerts)."""
    h = struct.unpack_from("<10f", range_bytes, 0)
    rec = np.frombuffer(range_bytes, dtype="<i2", count=nv * 10, offset=40).reshape(nv, 10).astype(np.float32)
    pos = np.array(h[0:3], np.float32) + rec[:, 0:3] * np.array(h[3:6], np.float32)
    uv = np.array(h[6:8], np.float32) + rec[:, 4:6] * np.array(h[8:10], np.float32)
    nrm = oct_decode(rec[:, 6:8])
    tan = np.zeros((nv, 4), np.float32)
    has = (rec[:, 3].astype(np.int16) & 1) != 0
    tan[has, :3] = oct_decode(rec[has, 8:10])
    tan[has, 3] = np.where((rec[has, 3].astype(np.int16) & 2) != 0, -1.0, 1.0)
    return pos, uv, nrm, tan


# ---------------------------------------------------------------------------------------
# Math helpers

def mat4(m):
    a = np.asarray(m, dtype=np.float64).reshape(4, 4)
    if not np.all(np.isfinite(a)):
        raise CaptureError("non-finite matrix")
    return a


def col_major(m):
    """Row-major numpy 4x4 (column vectors) -> the 16 floats of a column-major mat4."""
    return [float(v) for v in np.asarray(m, dtype=np.float64).T.reshape(16)]


def split_scale(m):
    """(rotation 3x3 orthonormal, scale xyz, translation) of an affine matrix."""
    lin = m[:3, :3]
    scale = np.linalg.norm(lin, axis=0)
    rot = lin / np.where(scale > 1e-12, scale, 1.0)
    # Orthonormalize (Gram-Schmidt) against shear; keep a right-handed frame.
    x = rot[:, 0] / max(np.linalg.norm(rot[:, 0]), 1e-12)
    y = rot[:, 1] - x * np.dot(x, rot[:, 1])
    y /= max(np.linalg.norm(y), 1e-12)
    z = np.cross(x, y)
    if np.dot(z, rot[:, 2]) < 0:
        scale = scale * np.array([1, 1, -1])
    return np.stack([x, y, z], axis=1), scale, m[:3, 3].copy()


def quat_from_matrix(r):
    """Unit quaternion (x, y, z, w) of a 3x3 rotation (column vectors)."""
    t = r[0, 0] + r[1, 1] + r[2, 2]
    if t > 0:
        s = math.sqrt(t + 1.0) * 2
        w, x, y, z = 0.25 * s, (r[2, 1] - r[1, 2]) / s, (r[0, 2] - r[2, 0]) / s, (r[1, 0] - r[0, 1]) / s
    elif r[0, 0] > r[1, 1] and r[0, 0] > r[2, 2]:
        s = math.sqrt(1.0 + r[0, 0] - r[1, 1] - r[2, 2]) * 2
        w, x, y, z = (r[2, 1] - r[1, 2]) / s, 0.25 * s, (r[0, 1] + r[1, 0]) / s, (r[0, 2] + r[2, 0]) / s
    elif r[1, 1] > r[2, 2]:
        s = math.sqrt(1.0 + r[1, 1] - r[0, 0] - r[2, 2]) * 2
        w, x, y, z = (r[0, 2] - r[2, 0]) / s, (r[0, 1] + r[1, 0]) / s, 0.25 * s, (r[1, 2] + r[2, 1]) / s
    else:
        s = math.sqrt(1.0 + r[2, 2] - r[0, 0] - r[1, 1]) * 2
        w, x, y, z = (r[1, 0] - r[0, 1]) / s, (r[0, 2] + r[2, 0]) / s, (r[1, 2] + r[2, 1]) / s, 0.25 * s
    n = math.sqrt(x * x + y * y + z * z + w * w)
    return [x / n, y / n, z / n, w / n]


def quat_rotate(q, v):
    x, y, z, w = q
    u = np.array([x, y, z])
    v = np.asarray(v, dtype=np.float64)
    return 2.0 * np.dot(u, v) * u + (w * w - np.dot(u, u)) * v + 2.0 * w * np.cross(u, v)


def sl_camera_from_blender(m):
    """SL camera frame (pos, quaternion x,y,z,w; at = +X, left = +Y, up = +Z) of a Blender
    camera world matrix (the camera looks down its local -Z with +Y up)."""
    rot, _, pos = split_scale(mat4(m))
    at, left, up = -rot[:, 2], -rot[:, 0], rot[:, 1]
    return [float(v) for v in pos], quat_from_matrix(np.stack([at, left, up], axis=1))


def perspective_rh_no(fov, aspect, z_near, z_far):
    """glm::perspective (right handed, depth -1..1), row-major for column vectors."""
    f = 1.0 / math.tan(fov * 0.5)
    d = z_far - z_near
    if abs(d) < 1e-12:
        d = 1e-12
    return np.array([[f / aspect, 0, 0, 0], [0, f, 0, 0],
                     [0, 0, -(z_far + z_near) / d, -(2.0 * z_far * z_near) / d], [0, 0, -1, 0]], dtype=np.float64)


# ---------------------------------------------------------------------------------------
# Skin weights (LLCleanQV::QWeight, 10 B: u8 joint[4], u16 weight[3], the 4th implied)

def encode_weights(joints, weights):
    """QWeight bytes (N * 10) of N vertices' four influences, the viewer's encodeWeight: weights
    normalised, apportioned to integers summing to exactly 65535 (floor, then the leftover units
    to the largest remainders, ties to the lower influence); zero-weight influences get joint 0;
    a vertex with no weight at all gets (1, 0, 0, 0) on its first joint."""
    j = np.asarray(joints, dtype=np.int64).reshape(-1, 4)
    w = np.asarray(weights, dtype=np.float64).reshape(-1, 4)
    n = len(j)
    w = np.where(np.isfinite(w) & (w > 0), w, 0.0)
    s = w.sum(axis=1)
    none = s <= 0
    w[none] = np.array([1.0, 0.0, 0.0, 0.0])
    s = np.where(none, 1.0, s)
    w = w / s[:, None]
    j = np.where(w > 0, np.clip(j, 0, 127), 0)
    sc = w * WEIGHT_ONE
    q = np.floor(sc)
    rem = sc - q
    left = (WEIGHT_ONE - q.sum(axis=1)).astype(np.int64)
    order = np.argsort(-rem, axis=1, kind="stable")
    rank = np.empty_like(order)
    np.put_along_axis(rank, order, np.tile(np.arange(4), (n, 1)), axis=1)
    q = q + (rank < left[:, None])
    rec = np.zeros(n, dtype=np.dtype([("j", "u1", 4), ("w", "<u2", 3)]))
    rec["j"] = j.astype(np.uint8)
    rec["w"] = q[:, :3].astype(np.uint16)
    assert rec.itemsize == SIZE["QWeight"]
    return rec.tobytes()


def decode_weights(blob, n):
    """(joints Nx4, weights Nx4 summing to 1) of QWeight bytes: the viewer's decodeWeight."""
    rec = np.frombuffer(blob, dtype=np.dtype([("j", "u1", 4), ("w", "<u2", 3)]), count=n)
    w3 = rec["w"].astype(np.int64)
    q3 = np.maximum(WEIGHT_ONE - w3.sum(axis=1), 0)
    w = np.concatenate([w3, q3[:, None]], axis=1) / float(WEIGHT_ONE)
    return (rec["j"] & 0x7F).astype(np.int64), w


# ---------------------------------------------------------------------------------------
# Records

def _f(vals, n):
    vals = [float(v) for v in vals]
    if len(vals) != n:
        raise CaptureError("expected %d floats, got %d" % (n, len(vals)))
    return vals


TRANSMISSION_MIN_ALPHA = 0.35   # alpha of a fully transmissive surface (Transmission = 1)


def transmission_to_alpha(base_alpha, transmission):
    """Principled transmission -> SL alpha. SL has no refraction: a see-through surface is an
    alpha-blended one. Alpha falls linearly from base_alpha (no transmission) to
    base_alpha * TRANSMISSION_MIN_ALPHA (full transmission), so the base colour stays a visible
    tint (glass, water) instead of vanishing."""
    t = min(max(float(transmission), 0.0), 1.0)
    return float(base_alpha) * (1.0 - t * (1.0 - TRANSMISSION_MIN_ALPHA))


def pack_instance(row, active, model, aabb_min, aabb_max, tint, lod_group, material, flags,
                  skin_id=0, spin_axis=(0, 0, 0, 0), spin_pivot=(0, 0, 0, 0)):
    uv_identity = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
    b = struct.pack("<II", row, 1 if active else 0)
    b += struct.pack("<16f", *_f(model, 16))
    b += struct.pack("<4f", *_f(list(aabb_min) + [0.0], 4))
    b += struct.pack("<4f", *_f(list(aabb_max) + [0.0], 4))
    b += struct.pack("<4f", *_f(tint, 4))
    b += struct.pack("<16f", *_f(uv_identity, 16))
    b += struct.pack("<4I", lod_group, material, flags, skin_id)
    b += struct.pack("<4f", *_f(spin_axis, 4))
    b += struct.pack("<4f", *_f(spin_pivot, 4))
    assert len(b) == SIZE["InstanceRec"]
    return b


def pack_material(mid, tex, emissive, metallic, roughness, alpha_cutoff, glow, alpha_blend, gltf=True):
    b = struct.pack("<I", mid)
    for t in tex:   # base, normal, mr, emissive (16 raw bytes each; zero = none)
        b += t if t else bytes(16)
    b += struct.pack("<4f", 1, 1, 1, 1)                 # base_color_factor: neutral (tint carries it)
    b += struct.pack("<4f", *_f(list(emissive) + [0.0], 4))
    b += struct.pack("<4f", 1, 1, 1, 0)                 # spec_color_gloss (legacy only)
    b += struct.pack("<5f", metallic, roughness, alpha_cutoff, 0.0, glow)
    b += struct.pack("<8B", 0, 0, 1 if gltf else 0, 0, 0, 1 if alpha_blend else 0, 0, 0)
    b += struct.pack("<4B", 0, 0, 0, 0)                 # uv_own[3] + pad: every map at the base UV
    b += struct.pack("<18f", *([1, 0, 0, 0, 1, 0] * 3))
    assert len(b) == SIZE["MaterialRec"]
    return b


def pack_mover(row, kind, model):
    b = struct.pack("<II", row, kind) + struct.pack("<16f", *_f(model, 16)) + struct.pack("<8f", *([0.0] * 8))
    assert len(b) == SIZE["MoverRec"]
    return b


def pack_env(sun):
    def v4(key, default):
        v = list(sun.get(key, default))[:4]
        return v + [0.0] * (4 - len(v))

    d = sun.get("direction", [0.40, 0.80, 0.50])
    n = math.sqrt(sum(float(x) * float(x) for x in d)) or 1.0
    b = struct.pack("<4f", *[float(x) / n for x in d] + [0.0])
    b += struct.pack("<4f", *v4("color", [1.00, 0.97, 0.90]))
    b += struct.pack("<4f", *v4("ambient", [0.30, 0.33, 0.40]))
    b += struct.pack("<4f", *v4("sky_horizon", [0.62, 0.72, 0.86]))
    b += struct.pack("<4f", *v4("sky_zenith", [0.18, 0.35, 0.66]))
    b += struct.pack("<4f", 0.02, 0.12, 0.18, 0.03)     # water_fog (the frame's default)
    b += bytes(4 * 64 + 4 * 24 + 32 + 4 * 16 + 32 + 4 * 40 + 96)   # atmos, water, normals, clouds, noise, bodies, sky_tex
    b += struct.pack("<f2i3I", 0.0, 0, 0, 0, 0, 0)      # probe ambiance, agent cell, probe/region counts, pad
    assert len(b) == SIZE["EnvRec"]
    return b


def light_record(L, tex_uuid_bytes, tex_width, key):
    """LLVKLight (256 B) for a point light or an SL projector, as the frame builder makes it."""
    m = mat4(L["matrix"])
    rot, scale, pos = split_scale(m)
    radius = float(L.get("radius", 10.0))
    s = radius * 1.5                                     # LL deferred radius (getLightRadius * 1.5)
    col = [float(c) * float(L.get("intensity", 1.0)) for c in L.get("color", [1, 1, 1])[:3]]
    falloff = float(L.get("falloff", 0.75)) * 0.5        # getLightFalloff(DEFERRED_LIGHT_FALLOFF 0.5)
    proj_params = [0.0] * 4
    proj_origin = [0.0] * 4
    proj_axis = [0.0] * 4
    proj_mat = [0.0] * 16
    shadow_vp = [0.0] * 16
    shadow_tan = [0.0, 0.0]
    shadow_near = 0.0
    tex = bytes(16)
    warnings = []
    if L.get("type") == "spot":
        fov = float(L.get("fov", 1.0))
        if not fov >= 0.01:
            fov = 0.01
        elif fov > 3.0:
            fov = 3.0
        sx, sy, sz = [abs(float(v)) for v in scale]
        at_axis = rot @ np.array([0.0, 0.0, -sz * 0.5])
        np_ = pos + at_axis
        at = rot @ np.array([0.0, 0.0, -1.0])
        dist = (sy * 0.5) / math.tan(fov * 0.5)
        origin = np_ - at * dist
        light_to_agent = np.eye(4)
        light_to_agent[:3, :3] = rot
        light_to_agent[:3, 3] = origin
        inv = np.linalg.inv(light_to_agent)
        near_clip = dist
        far_clip = s + dist - sz
        if far_clip <= near_clip:
            warnings.append("projector %s: radius*1.5 (%.3f) <= prim depth: empty frustum (the viewer does the same)"
                            % (L.get("name"), s))
        aspect = sx / sy if sy > 0 else 1.0
        trans = np.array([[0.5, 0, 0, 0.5], [0, 0.5, 0, 0.5], [0, 0, 0.5, 0.5], [0, 0, 0, 1]], dtype=np.float64)
        proj_mat = col_major(trans @ perspective_rh_no(fov, aspect, near_clip, far_clip) @ inv)
        sn = near_clip + max(0.01, near_clip * 0.01)
        sf = max(far_clip, sn + 0.01)
        gl_to_vk = np.eye(4)
        gl_to_vk[1, 1] = -1.0
        gl_to_vk[2, 2] = 0.5
        gl_to_vk[2, 3] = 0.5
        shadow_vp = col_major(gl_to_vk @ perspective_rh_no(fov, aspect, sn, sf) @ inv)
        shadow_tan = [math.tan(fov * 0.5) * aspect, math.tan(fov * 0.5)]
        shadow_near = sn
        lod_range = math.log2(max(tex_width, 1)) if tex_uuid_bytes else 0.0
        if tex_uuid_bytes:
            tex = tex_uuid_bytes
        proj_params = [float(L.get("focus", 0.0)), max(far_clip - near_clip, 0.001), lod_range,
                       float(L.get("ambiance", 0.0))]
        proj_origin = [float(origin[0]), float(origin[1]), float(origin[2]), 1.0]
        proj_axis = [float(at[0]), float(at[1]), float(at[2]), near_clip]
    else:
        r = 0.5 * float(np.linalg.norm(scale))           # the bulb prim's bounding radius
        proj_axis[0] = min(r, 64.0) if r > 0 else 0.0
    b = struct.pack("<4f", float(pos[0]), float(pos[1]), float(pos[2]), s)
    b += struct.pack("<4f", col[0], col[1], col[2], falloff)
    b += struct.pack("<4f", *proj_params) + struct.pack("<4f", *proj_origin) + struct.pack("<4f", *proj_axis)
    b += struct.pack("<16f", *proj_mat) + tex
    b += struct.pack("<16f", *shadow_vp) + struct.pack("<2f", *shadow_tan) + struct.pack("<f", shadow_near)
    b += bytes(4) + struct.pack("<Q", key) + struct.pack("<B", 0) + bytes(7)
    assert len(b) == SIZE["LLVKLight"]
    # Floats 0..35 (pos .. proj_mat) and 40..58 (shadow_vp .. shadow_near); 36..39 is the UUID.
    floats = struct.unpack_from("<36f", b, 0) + struct.unpack_from("<19f", b, 160)
    for v in floats:
        if not math.isfinite(v):
            raise CaptureError("light %s: non-finite record (degenerate fixture scale or rotation?)" % L.get("name"))
    return b, warnings


def file_header(kind, count, version=FORMAT_VERSION):
    return MAGIC + struct.pack("<IIQ", version, kind, count)


def texture_uuid(name):
    return uuid.uuid5(UUID_NAMESPACE, "texture:" + name)


def light_key(name):
    k = xxh3_64(("light:" + name).encode("utf-8")) & 0x7FFFFFFFFFFFFFFF
    return k or 1


# ---------------------------------------------------------------------------------------
# Camera path

def camera_path_json(camera):
    keys = []
    prev_segment = None
    for k in sorted(camera.get("keys", []), key=lambda k: float(k["t"])):
        if "matrix" in k:
            pos, rot = sl_camera_from_blender(k["matrix"])
        else:
            pos, rot = [float(v) for v in k["pos"]], [float(v) for v in k["rot"]]
        o = {"t": float(k["t"]), "pos": pos, "rot": rot, "fov": float(k.get("fov", 1.0471976))}
        seg = k.get("segment") or prev_segment or "default"
        if seg != prev_segment:
            o["segment"] = seg
        prev_segment = seg
        if int(k.get("still", 0)) > 0:
            o["still"] = int(k["still"])
        if k.get("name"):
            o["name"] = str(k["name"])   # ignored by the viewer's parser; bench.py names still frames with it
        keys.append(o)
    if not keys:
        raise CaptureError("the camera path has no keys")
    return {"format": "slipstream-bench-path", "version": 1, "keys": keys}


# ---------------------------------------------------------------------------------------
# The writer

def _norm_tracks(n_samples, track):
    if track is None:
        return None
    if len(track) < n_samples:
        track = list(track) + [track[-1]] * (n_samples - len(track))
    return track[:n_samples]


def write_capture(scene, out_dir, overwrite=False, log=print):
    """Write `scene` as a capture into out_dir. Returns a summary dict."""
    if np is None:
        raise CaptureError("numpy is required")
    if os.path.exists(os.path.join(out_dir, "manifest.json")) and not overwrite:
        raise CaptureError(out_dir + " already holds a capture")
    os.makedirs(os.path.join(out_dir, "textures"), exist_ok=True)
    warnings = []
    hz = float(scene.get("track_hz", 60.0))
    if not 1.0 <= hz <= 1000.0:
        raise CaptureError("track_hz must be within 1..1000 (the replay clamps it)")
    seconds = float(scene.get("seconds", 1.0 / hz))
    n_samples = max(1, int(round(seconds * hz)))
    skins = list(scene.get("skins", []))
    armatures = list(scene.get("armatures", []))
    ver = FORMAT_VERSION_SKINS if skins else FORMAT_VERSION
    _check_skins(scene, skins, armatures)

    # Textures: uuid5 of the name, PNG files, manifest entries.
    tex_ids, tex_info, tex_files = {}, [], {}
    for t in scene.get("textures", []):
        u = texture_uuid(t["name"])
        w, h, c = int(t["width"]), int(t["height"]), int(t["channels"])
        tex_ids[t["name"]] = (u.bytes, w)
        rel = "textures/%s.png" % str(u)
        data = png_bytes(w, h, c, bytes(t["pixels"]))
        with open(os.path.join(out_dir, rel), "wb") as f:
            f.write(data)
        tex_files[rel] = data
        tex_info.append({"id": str(u), "w": w, "h": h, "c": c, "discard": 0, "name": t["name"]})
        if (w & (w - 1)) or (h & (h - 1)):
            warnings.append("texture %s is %dx%d (not a power of two)" % (t["name"], w, h))

    def tex_ref(name, what):
        if not name:
            return None
        if name not in tex_ids:
            warnings.append("%s names texture %r, which is not exported (drawn without it)" % (what, name))
            return None
        return tex_ids[name][0]

    # Materials: ids from 1 (0 is the replay's default white).
    materials = scene.get("materials", [])
    mat_blob = bytearray()
    mat_alpha = []
    for i, m in enumerate(materials):
        mode = m.get("alpha_mode", "OPAQUE").upper()
        cutoff = float(m.get("alpha_cutoff", 0.5)) if mode == "MASK" else -1.0
        strength = float(m.get("emissive_strength", 1.0))
        em = [float(c) * strength for c in m.get("emissive", [0, 0, 0])[:3]]
        tx = m.get("textures", {})
        tex = [tex_ref(tx.get(k), "material %s (%s)" % (m.get("name"), k)) for k in ("base", "normal", "mr", "emissive")]
        mat_blob += pack_material(i + 1, tex, em, float(m.get("metallic", 0.0)), float(m.get("roughness", 1.0)),
                                  cutoff, float(m.get("glow", 0.0)), mode == "BLEND")
        flags = (FLAG_ALPHA if mode == "BLEND" else 0) | (FLAG_MASK if mode == "MASK" else 0)
        if m.get("fullbright"):
            flags |= FLAG_FULLBRIGHT
        mat_alpha.append(flags)

    # Geometry: one slot per face chunk; one LOD group per slot (highest level only).
    slot_blob = bytearray()
    groups = []          # (gid, slot)
    mesh_faces = {}      # mesh_key -> [(gid, material index, aabb_lo, aabb_hi)]
    mesh_maxj = {}       # mesh_key -> highest skin joint index any vertex names (-1: no weights)
    next_slot = 1
    for key, faces in scene.get("meshes", {}).items():
        out = []
        for fi, face in enumerate(faces):
            pos = np.asarray(face["positions"], dtype=np.float32).reshape(-1, 3)
            idx = np.asarray(face["indices"], dtype=np.int64).reshape(-1)
            if len(idx) % 3 or len(idx) == 0:
                raise CaptureError("mesh %s face %d: index count %d is not a positive multiple of 3" % (key, fi, len(idx)))
            if idx.min() < 0 or idx.max() >= len(pos):
                raise CaptureError("mesh %s face %d: index out of range" % (key, fi))
            nrm = face.get("normals")
            uvs = face.get("uvs")
            tan = face.get("tangents")
            nrm = np.asarray(nrm, np.float32).reshape(-1, 3) if nrm is not None else None
            uvs = np.asarray(uvs, np.float32).reshape(-1, 2) if uvs is not None else None
            tan = np.asarray(tan, np.float32).reshape(-1, 4) if tan is not None else None
            fj, fw = face.get("joints"), face.get("weights")
            if (fj is None) != (fw is None):
                raise CaptureError("mesh %s face %d: joints and weights come together" % (key, fi))
            if fj is not None:
                fj = np.asarray(fj, np.int64).reshape(-1, 4)
                fw = np.asarray(fw, np.float64).reshape(-1, 4)
                if len(fj) != len(pos) or len(fw) != len(pos):
                    raise CaptureError("mesh %s face %d: joints / weights do not match the vertices" % (key, fi))
                if fj.min() < 0 or fj.max() >= MAX_SKIN_JOINTS:
                    raise CaptureError("mesh %s face %d: skin joint index out of range" % (key, fi))
                mesh_maxj[key] = max(mesh_maxj.get(key, -1), int(fj[fw > 0].max()) if (fw > 0).any() else 0)
            for chunk_idx in _chunk_triangles(idx, MAX_SLOT_VERTS):
                used, local = np.unique(chunk_idx, return_inverse=True)
                cp = pos[used]
                rng, _ = quantize_face(cp, nrm[used] if nrm is not None else None,
                                       uvs[used] if uvs is not None else None, tan[used] if tan is not None else None)
                slot = next_slot
                next_slot += 1
                nv, ni = len(used), len(local)
                slot_blob += struct.pack("<4I", slot, nv, ni, 1 if fj is not None else 0) + rng + local.astype("<u2").tobytes()
                if fj is not None:
                    slot_blob += encode_weights(fj[used], fw[used])
                gid = len(groups)
                groups.append((gid, slot))
                out.append((gid, int(face.get("material", 0)), cp.min(axis=0), cp.max(axis=0)))
        mesh_faces[key] = out

    # Rows (one per face chunk per object) and the mover track.
    inst_blob = bytearray()
    rows = []            # (row, object index, gid, mat index, lo, hi)
    obj_rows = {}
    objects = scene.get("objects", [])
    for oi, o in enumerate(objects):
        if o["mesh"] not in mesh_faces:
            raise CaptureError("object %s names mesh %r, which is not in meshes" % (o["name"], o["mesh"]))
        obj_rows[oi] = []
        if o.get("skin") is not None:
            if mesh_maxj.get(o["mesh"], -1) < 0:
                raise CaptureError("object %s has a skin but its mesh carries no weights" % o["name"])
            if mesh_maxj[o["mesh"]] >= len(skins[int(o["skin"])]["joint_nums"]):
                raise CaptureError("object %s: a vertex names a joint beyond its skin's %d joints"
                                   % (o["name"], len(skins[int(o["skin"])]["joint_nums"])))
        elif mesh_maxj.get(o["mesh"], -1) >= 0:
            raise CaptureError("object %s: its mesh has weights but the object has no skin" % o["name"])
        for gid, mi, lo, hi in mesh_faces[o["mesh"]]:
            r = len(rows)
            rows.append((r, oi, gid, mi, lo, hi))
            obj_rows[oi].append(r)
    if not rows:
        raise CaptureError("no geometry (the viewer refuses a capture without static rows)")

    tracks = {oi: _norm_tracks(n_samples, o.get("track")) for oi, o in enumerate(objects)}
    mover_samples = []   # (sample, [(row, kind, model)])
    moved_rows = set()
    last = {oi: col_major(mat4(o["matrix"])) for oi, o in enumerate(objects)}
    gone = set()
    for s in range(n_samples):
        changed = []
        for oi, tr in tracks.items():
            if tr is None or oi in gone:
                continue
            m = tr[s]
            if m is None:
                for r in obj_rows[oi]:
                    changed.append((r, MOVER_GONE, last[oi]))
                gone.add(oi)
                continue
            cm = col_major(mat4(m))
            if cm != last[oi]:
                for r in obj_rows[oi]:
                    changed.append((r, MOVER_POSE, cm))
                    moved_rows.add(r)
                last[oi] = cm
        if changed or s == 0 or s == n_samples - 1:
            mover_samples.append((s, changed))

    rigged_rows = 0
    for r, oi, gid, mi, lo, hi in rows:
        o = objects[oi]
        if not 0 <= mi < len(materials):
            warnings.append("object %s: material index %d out of range (default white)" % (o["name"], mi))
            mid, flags, tint = 0, 0, [1, 1, 1, 1]
        else:
            m = materials[mi]
            mid, flags = mi + 1, mat_alpha[mi]
            tint = (list(m.get("base_color", [1, 1, 1, 1])) + [1.0])[:4]
        if o.get("no_shadow"):
            flags |= FLAG_EMISSIVE_NO_SHADOW
        skin_id = 0
        if o.get("skin") is not None:
            flags |= FLAG_RIGGED
            skin_id = int(o["skin"])
            rigged_rows += 1
        inst_blob += pack_instance(r, r in moved_rows, col_major(mat4(o["matrix"])), lo, hi, tint, gid, mid, flags,
                                   skin_id=skin_id)

    group_blob = bytearray()
    for gid, slot in groups:
        group_blob += struct.pack("<5I", gid, SENTINEL, SENTINEL, SENTINEL, slot)

    mover_blob = bytearray()
    for s, recs in mover_samples:
        mover_blob += struct.pack("<II", s, len(recs))
        for r, kind, model in recs:
            mover_blob += pack_mover(r, kind, model)

    # Lights, per sample; consecutive identical samples are written once (the replay holds a
    # record until the next one), the last sample always (it sets the loop length).
    lights = scene.get("lights", [])
    ltracks = [_norm_tracks(n_samples, L.get("track")) for L in lights]
    light_samples = []
    prev = None
    lights_max = 0
    for s in range(n_samples):
        recs = []
        for L, tr in zip(lights, ltracks):
            cur = dict(L)
            if tr is not None and tr[s]:
                cur.update(tr[s])
            if not cur.get("enabled", True):
                continue
            gobo = cur.get("gobo")
            tid = tex_ids.get(gobo) if gobo else None
            if gobo and tid is None:
                warnings.append("light %s: gobo %r not exported (white)" % (cur.get("name"), gobo))
            rec, w = light_record(cur, tid[0] if tid else None, tid[1] if tid else 0, light_key(cur["name"]))
            if s == 0:
                warnings.extend(w)
            recs.append(rec)
        lights_max = max(lights_max, len(recs))
        blob = b"".join(recs)
        if blob != prev or s == n_samples - 1:
            light_samples.append((s, len(recs), blob))
        prev = blob
    if lights_max > VIEWER_MAX_LIGHTS:
        warnings.append("%d lights in a sample: the old pipeline keeps %d per frame" % (lights_max, VIEWER_MAX_LIGHTS))
    light_blob = bytearray()
    for s, n, blob in light_samples:
        light_blob += struct.pack("<II", s, n) + blob

    # Environment.
    env_blob = pack_env(scene.get("sun", {}))
    env_texts = {}
    env_from = scene.get("env_from")
    if env_from:
        env_blob, env_texts, extra_tex = _borrow_env(env_from, out_dir, warnings)
        have = {t["id"] for t in tex_info}
        tex_info.extend(t for t in extra_tex if t["id"] not in have)

    files = {}

    def put(name, data):
        with open(os.path.join(out_dir, name), "wb") as f:
            f.write(data)
        files[name] = {"bytes": len(data), "xxh64": hex64(xxh3_64(data))}

    put("instances.bin", file_header(KIND_INSTANCES, len(rows), ver) + bytes(inst_blob))
    put("groups.bin", file_header(KIND_GROUPS, len(groups), ver) + bytes(group_blob))
    put("slots.bin", file_header(KIND_SLOTS, len(groups), ver) + bytes(slot_blob))
    put("materials.bin", file_header(KIND_MATERIALS, len(materials), ver) + bytes(mat_blob))
    put("terrain.bin", file_header(KIND_TERRAIN, 0, ver))
    put("env.bin", file_header(KIND_ENV, 1, ver) + env_blob)
    put("lights.bin", file_header(KIND_LIGHTS, len(light_samples), ver) + bytes(light_blob))
    put("movers.bin", file_header(KIND_MOVERS, len(mover_samples), ver) + bytes(mover_blob))
    pose_samples = 0
    if skins:
        skin_blob = b"".join(pack_skin(i, k) for i, k in enumerate(skins))
        put("skins.bin", file_header(KIND_SKINS, len(skins), ver) + skin_blob)
        pose_blob, pose_samples = pose_track(armatures, n_samples)
        put("poses.bin", file_header(KIND_POSES, pose_samples, ver) + pose_blob)
    put("camera.json", (json.dumps(camera_path_json(scene.get("camera", {})), separators=(",", ":")) + "\n").encode())
    for name, data in env_texts.items():
        put(name, data)
    for rel, data in tex_files.items():
        files[rel] = {"bytes": len(data), "xxh64": hex64(xxh3_64(data))}
    for t in tex_info:
        rel = "textures/%s.png" % t["id"]
        if rel not in files and os.path.isfile(os.path.join(out_dir, rel)):
            with open(os.path.join(out_dir, rel), "rb") as f:
                data = f.read()
            files[rel] = {"bytes": len(data), "xxh64": hex64(xxh3_64(data))}

    unsupported = list(scene.get("unsupported", []))
    sidecar = {"format": "slipstream-bench-synthetic", "version": 1, "unsupported": unsupported,
               "lights": [{k: v for k, v in L.items() if k not in ("track", "matrix")} for L in lights],
               "sun": scene.get("sun", {}), "emissive_meshes": _emissive_list(scene),
               "extras": scene.get("extras", {}), "warnings": warnings}
    put("synthetic.json", (json.dumps(sidecar, indent=1, default=_json_default) + "\n").encode())

    missing = [m for m in VIEWER_MISSING if not (skins and m.startswith("avatars"))]
    if not env_from:
        missing.append("environment: only sun direction / colour, ambient and sky gradient are set "
                       "(atmosphere, water, clouds, sky textures are zero; see synthetic.json)")
    kinds = sorted({u.get("kind", "?") for u in unsupported})
    for k in kinds:
        names = [u.get("name", "?") for u in unsupported if u.get("kind") == k]
        missing.append("synthetic: %s (%d: %s) -> synthetic.json" % (k, len(names), ", ".join(names[:8])))

    manifest = {
        "format": "slipstream-bench-capture", "version": ver,
        "created": time.strftime("%Y%m%d-%H%M%S"), "viewer": "render2 synthetic exporter",
        "region": scene.get("name", "synthetic"), "seconds": n_samples / hz, "track_hz": hz, "truncated": False,
        "synthetic": True,
        "counts": {"instances": len(rows), "skipped_rigged": 0, "skipped_hud": 0, "skipped_worn": 0,
                   "groups": len(groups), "slots": len(groups), "slots_missing": 0, "materials": len(materials),
                   "terrain_records": 0, "probes": 0, "probe_regions": 0, "light_samples": len(light_samples),
                   "lights_max": lights_max, "mover_samples": len(mover_samples), "textures": len(tex_info),
                   "textures_missing": 0},
        "missing": missing,
        "textures": [{k: t[k] for k in ("id", "w", "h", "c", "discard")} for t in tex_info],
        "files": files,
        "bytes": sum(v["bytes"] for v in files.values()),
    }
    if skins:
        manifest["counts"].update({"skins": len(skins), "armatures": len(armatures), "pose_samples": pose_samples,
                                   "rigged_rows": rigged_rows})
    text = json.dumps(manifest, separators=(",", ":"))
    tmp = os.path.join(out_dir, "manifest.json.tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text)
    os.replace(tmp, os.path.join(out_dir, "manifest.json"))   # last: a directory without one is incomplete
    for w in warnings:
        log("warning: " + w)
    return {"dir": out_dir, "capture_id": hex64(xxh3_64(text.encode("utf-8"))), "rows": len(rows),
            "slots": len(groups), "materials": len(materials), "textures": len(tex_info),
            "light_samples": len(light_samples), "lights_max": lights_max, "mover_samples": len(mover_samples),
            "skins": len(skins), "armatures": len(armatures), "pose_samples": pose_samples, "rigged_rows": rigged_rows,
            "version": ver, "warnings": warnings}


def _check_skins(scene, skins, armatures):
    if len(skins) > MAX_SKINS or len(armatures) > MAX_ARMATURES:
        raise CaptureError("too many skins (%d, limit %d) or armatures (%d, limit %d)"
                           % (len(skins), MAX_SKINS, len(armatures), MAX_ARMATURES))
    if skins and not armatures:
        raise CaptureError("skins without armatures")
    for ai, a in enumerate(armatures):
        jc = int(a["joint_count"])
        if not 1 <= jc <= MAX_ARMATURE_JOINTS:
            raise CaptureError("armature %s: %d joints (limit %d)" % (a.get("name"), jc, MAX_ARMATURE_JOINTS))
        if not a.get("track"):
            raise CaptureError("armature %s has no pose" % a.get("name"))
    for si, k in enumerate(skins):
        jn = list(k["joint_nums"])
        ai = int(k["armature"])
        if not 0 <= ai < len(armatures):
            raise CaptureError("skin %s: armature %d does not exist" % (k.get("name"), ai))
        if not 1 <= len(jn) <= MAX_SKIN_JOINTS:
            raise CaptureError("skin %s: %d joints (limit %d)" % (k.get("name"), len(jn), MAX_SKIN_JOINTS))
        if min(jn) < 0 or max(jn) >= int(armatures[ai]["joint_count"]):
            raise CaptureError("skin %s: a joint number is outside armature %s" % (k.get("name"), armatures[ai].get("name")))
        if len(k["inv_bind"]) != len(jn):
            raise CaptureError("skin %s: %d inverse binds for %d joints" % (k.get("name"), len(k["inv_bind"]), len(jn)))
        for m in list(k["inv_bind"]) + [k["bind_shape"]]:
            mat4(m)
    for o in scene.get("objects", []):
        if o.get("skin") is not None and not 0 <= int(o["skin"]) < len(skins):
            raise CaptureError("object %s names skin %s, which does not exist" % (o.get("name"), o["skin"]))


def pack_skin(skin_id, k):
    jn = [int(v) for v in k["joint_nums"]]
    jc = len(jn)
    ib = np.zeros((MAX_SKIN_JOINTS, 16), dtype="<f4")
    for j, m in enumerate(k["inv_bind"]):
        ib[j] = col_major(mat4(m))
    bs = mat4(k["bind_shape"])
    bs = bs.copy()
    bs[3] = [0.0, 0.0, 0.0, 1.0]
    b = struct.pack("<4I", skin_id, int(k["armature"]), jc, 0)
    b += struct.pack("<16f", *col_major(bs))
    b += struct.pack("<%di" % MAX_SKIN_JOINTS, *(jn + [0] * (MAX_SKIN_JOINTS - jc)))
    b += ib.tobytes()
    assert len(b) == SIZE["SkinRec"]
    return b


def pose_track(armatures, n_samples):
    """(poses.bin payload, sample count): per sample only the armatures whose joint world matrices
    changed; sample 0 lists them all and the last sample is always written (the loop length)."""
    tracks = [_norm_tracks(n_samples, a["track"]) for a in armatures]
    last = [None] * len(armatures)
    blob = bytearray()
    count = 0
    for s in range(n_samples):
        recs = []
        for ai, a in enumerate(armatures):
            jc = int(a["joint_count"])
            m = np.asarray(tracks[ai][s], dtype=np.float64).reshape(jc, 4, 4)
            if not np.all(np.isfinite(m)):
                raise CaptureError("armature %s: non-finite pose at sample %d" % (a.get("name"), s))
            data = np.ascontiguousarray(m[:, :3, :].reshape(jc, 12), dtype="<f4").tobytes()   # rows 0..2, row-major
            if data != last[ai]:
                recs.append(struct.pack("<4I", ai, jc, 0, 0) + data)
                last[ai] = data
        if recs or s == 0 or s == n_samples - 1:
            blob += struct.pack("<II", s, len(recs)) + b"".join(recs)
            count += 1
    return bytes(blob), count


def _chunk_triangles(idx, max_verts):
    """Split a triangle list into runs that each use at most max_verts distinct vertices."""
    if len(np.unique(idx)) <= max_verts:
        return [idx]
    out, cur, seen = [], [], set()
    tris = idx.reshape(-1, 3)
    for t in tris:
        new = [v for v in t if v not in seen]
        if len(seen) + len(new) > max_verts:
            out.append(np.asarray(cur, dtype=np.int64).reshape(-1))
            cur, seen = [], set()
            new = list(t)
        cur.append(t)
        seen.update(int(v) for v in new)
    if cur:
        out.append(np.asarray(cur, dtype=np.int64).reshape(-1))
    return out


def _emissive_list(scene):
    out = []
    mats = scene.get("materials", [])
    for o in scene.get("objects", []):
        for fi, face in enumerate(scene.get("meshes", {}).get(o["mesh"], [])):
            mi = int(face.get("material", -1))
            if 0 <= mi < len(mats):
                m = mats[mi]
                em = [float(c) * float(m.get("emissive_strength", 1.0)) for c in m.get("emissive", [0, 0, 0])[:3]]
                if max(em) > 0.0:
                    out.append({"object": o["name"], "face": fi, "material": m.get("name"), "emissive": em})
    return out


def _json_default(o):
    if np is not None and isinstance(o, np.ndarray):
        return o.tolist()
    if np is not None and isinstance(o, np.generic):
        return o.item()
    return str(o)


def _borrow_env(src, out_dir, warnings):
    """env.bin, sky.xml, water.xml and the environment's textures from a real capture."""
    with open(os.path.join(src, "manifest.json"), encoding="utf-8") as f:
        sman = json.load(f)
    with open(os.path.join(src, "env.bin"), "rb") as f:
        raw = f.read()
    if raw[:8] != MAGIC or struct.unpack_from("<II", raw, 8) != (FORMAT_VERSION, KIND_ENV):
        raise CaptureError("%s/env.bin is not a version %d env file" % (src, FORMAT_VERSION))
    payload = raw[24:]
    env = bytearray(payload[:SIZE["EnvRec"]])
    # Keep the environment, drop the probes and regions (they describe the other scene).
    struct.pack_into("<2i3I", env, 836, 0, 0, 0, 0, 0)
    if struct.unpack_from("<I", payload, 844)[0]:
        warnings.append("env_from: the source capture's manual probes and probe regions are not copied")
    texts = {}
    for name in ("sky.xml", "water.xml"):
        p = os.path.join(src, name)
        if os.path.isfile(p):
            with open(p, "rb") as f:
                texts[name] = f.read()
    ids = set()
    # Offsets of the UUID arrays inside EnvRec: water_normal 448, cloud_noise 544, sky_tex 736.
    for off, n in ((96 + 256 + 96, 2), (96 + 256 + 96 + 32 + 64, 2), (96 + 256 + 96 + 32 + 64 + 32 + 160, 6)):
        for k in range(n):
            b = bytes(env[off + 16 * k: off + 16 * (k + 1)])
            if any(b):
                ids.add(str(uuid.UUID(bytes=b)))
    extra = []
    known = {t["id"]: t for t in sman.get("textures", [])}
    for i in sorted(ids):
        p = os.path.join(src, "textures", i + ".png")
        if os.path.exists(os.path.join(out_dir, "textures", i + ".png")):
            continue   # the scene exports this texture itself (same id): keep its own file
        if os.path.isfile(p) and i in known:
            shutil.copyfile(p, os.path.join(out_dir, "textures", i + ".png"))
            t = dict(known[i])
            t.setdefault("discard", 0)
            t["name"] = "env:" + i
            extra.append(t)
        else:
            warnings.append("env_from: environment texture %s is not in the source capture" % i)
    return bytes(env), texts, extra
