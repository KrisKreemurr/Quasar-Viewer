#!/usr/bin/env python3
"""Source-contract checks for local-only HDR capture routing."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]

snap = (ROOT / "indra/newview/vkgfx/llcleansnapshotvk.cpp").read_text()
snap_h = (ROOT / "indra/newview/vkgfx/llcleansnapshotvk.h").read_text()
ctx = (ROOT / "indra/newview/vkgfx/llvulkancontext.cpp").read_text()
ctx_h = (ROOT / "indra/newview/vkgfx/llvulkancontext.h").read_text()
viewer = (ROOT / "indra/newview/llviewerwindow.cpp").read_text()
viewer_h = (ROOT / "indra/newview/llviewerwindow.h").read_text()
preview = (ROOT / "indra/newview/llsnapshotlivepreview.cpp").read_text()
local = (ROOT / "indra/newview/llpanelsnapshotlocal.cpp").read_text()
renderer = (ROOT / "indra/newview/vkgfx/llcleanrenderervk.cpp").read_text()
matte = (ROOT / "indra/newview/vkgfx/llcleansnapmattevk.cpp").read_text()
model = (ROOT / "indra/newview/llsnapshotmodel.h").read_text()
shader = (ROOT / "indra/vkclean/shaders/clean_snap_live.comp").read_text()
regen = (ROOT / "indra/vkclean/regen_world_spv.py").read_text()
limits = (ROOT / "indra/newview/vkgfx/llsnapshotbufferlimits.h").read_text()
image_impl = (ROOT / "indra/llimage/llimage.cpp").read_text()

assert "r.pairedHdr && r.mode != MODE_RESAMPLE && r.mode != MODE_ACCUM" in snap
assert "(r.hdrRgbeOutput || r.pairedHdr) && s.swapFormat != VK_FORMAT_R16G16B16A16_SFLOAT" in snap
assert "packedRgbeBytes" in snap and "mReq.hdrRgbeOutput ? 4u : 3u" in snap
assert "r.hdrRgbeOutput) pc.aux[0] |= kFlagHdrRgbe" in snap
assert "mPipe[r.hdrRgbeOutput ? PIPE_HDR_RGBE : PIPE_V]" in snap
assert "kWorldSnapLiveHDRSpv" in snap_h or "kWorldSnapLiveHDRSpv" in snap
assert "mHdrOutputActive ? VK_FORMAT_R16G16B16A16_SFLOAT" in ctx
assert "bool hdrDisplayActive() const" in ctx_h
assert "req.hdrRgbeOutput = hdrRgbeOutput" in viewer
assert "req.point = show_ui ? 3u : (show_hud ? 2u : 1u)" in viewer
assert "outputCopies =" not in viewer
assert "LLRadianceSnapshot::serialize" in preview
assert 'req.pairedHdr = pairedHdr != nullptr' in viewer
assert 'mPreviewHdrRgbe.clear()' in preview and 'mPreviewHdrRgbe = std::move(pairedHdr)' in preview
assert 'SnapshotSaveHdrCopy' in preview and 'local_save_hdr_copy' in local
assert 'format->add(hdrLabel' not in local and 'rawSnapshotHDR' not in preview
assert 'LLVulkanContext::instance().hdrDisplayActive()' in local
assert 'mSnapshotBufferType == LLSnapshotModel::SNAPSHOT_TYPE_COLOR' in preview
assert 'mSetHdr' in snap and 'mWorkHdr' in snap and 'pc.aux[0] |= kFlagHdrRgbe' in snap
assert 'PIPE_HDR_RESOLVE' in snap and 'kWorldSnapLiveHDRResolveSpv' in regen
assert 'if (r.pairedHdr)' in snap and 'mOutHdr.mapped' in snap
assert 'saveHdrSidecarExclusive(sidecar.get(), filepath.substr' in viewer
assert 'APR_FOPEN_EXCL' in viewer and 'LLFile::remove(path)' in viewer
assert 'requiredBytes += sidecarBytes' in viewer
assert 'mTonemapAlphaHdrPipe = createGraphicsPipeline' in matte
assert 'if (mSnapHdrView && mSnapMatte && mSnapHoldRan && mTonemapAlphaHdrPipe)' in renderer
assert 'mScopeCaptureFormat = mScopeImg.format' in renderer and 'scopeBytesPerPixel' in renderer
assert 'SNAPSHOT_TYPE_AVATAR_TRANSPARENT' in local and 'SNAPSHOT_TYPE_AVATAR_TRANSPARENT' in preview
assert 'Never show or save a previous layer if the new capture fails' in preview
assert "SNAPSHOT_FORMAT_HDR" not in model
fmt = local[local.index("LLSnapshotModel::ESnapshotFormat LLPanelSnapshotLocal::getImageFormat"):local.index("void LLPanelSnapshotLocal::updateControls")]
assert 'if (id == "PNG")' in fmt and 'id == "HDR"' not in fmt
assert "SNAPSHOT_FORMAT_PNG" in fmt  # unrecognized/local-only choice resolves as SDR PNG
encoded = preview[preview.index("LLPointer<LLImageRaw> LLSnapshotLivePreview::getEncodedImage"):preview.index("bool LLSnapshotLivePreview::createUploadFile")]
assert "SNAPSHOT_FORMAT_HDR" not in encoded
formatted = preview[preview.index("LLPointer<LLImageFormatted> LLSnapshotLivePreview::getFormattedImage"):preview.index("void LLSnapshotLivePreview::setSnapshotFormat")]
assert "SNAPSHOT_FORMAT_HDR" not in formatted and "SNAPSHOT_FORMAT_PNG" in formatted
assert "snapshotPeakSystemBytes" in viewer
assert "outputCopies" in limits and "max - srcBytes" in limits
assert "LLRadianceSnapshot::peakHostBytes" in viewer
assert "maxSerializedBytes" in (ROOT / "indra/newview/vkgfx/llradiancesnapshot.h").read_text()
assert "S32 result = outfile.write(getData(), getDataSize())" in image_impl
assert '"kWorldSnapLiveHDRSpv"' in regen and "-DSNAP_HDR_RGBE=1" in regen
assert "if ((pc.aux.x & kHdrRgbe) != 0u) return max(nits, vec3(0.0))" in shader
assert "outp[f] = encodeRgbe(c)" in shader and "SNAP_HDR_RGBE" in shader
assert "dstH - 1u - f / dstW" in shader
assert "const size_t bytes = (size_t)w * h * (mReq.hdrRgbeOutput ? 4u : 3u)" in snap
print("HDR snapshot source contract checks passed")
