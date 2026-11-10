/**
 * @file sltelemetrysettings.h
 * @author SkyLine
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 * $/LicenseInfo$
 */
// Closed numeric/boolean settings; generated from the canonical schema.
#pragma once
#include <array>
namespace SLTelemetry {
struct SettingRule { const char* name; unsigned type; double lower, upper, step; std::array<unsigned,16> buckets; unsigned bucketCount; };
inline constexpr SettingRule SETTING_RULES[] = {
    {"RenderVKDlssRayReconstruction",1,0,2,1, {0,1,2},3},
    {"RenderVKSunShadows",1,0,2,1, {0,1,2},3},
    {"RenderVKProjectorShadows",1,0,2,1, {0,1,2},3},
    {"RenderScreenSpaceReflections",0,0,1,1, {},0},
    {"RenderMirrors",0,0,1,1, {},0},
    {"RenderVKTAA",0,0,1,1, {},0},
    {"RenderVKGodRays",1,0,3,1, {0,1,2,3},4},
    {"RenderVKRayTracing",1,0,1,1, {0,1},2},
    {"RenderDeferredSSAO",0,0,1,1, {},0},
    {"RenderVKAOStandard",0,0,1,1, {},0},
    {"RenderTransparentWater",0,0,1,1, {},0},
    {"RenderDepthOfField",0,0,1,1, {},0},
    {"RenderVSyncEnable",0,0,1,1, {},0},
    {"RenderVKFramePacing",0,0,1,1, {},0},
    {"RenderHiDPI",0,0,1,1, {},0},
    {"RenderVKTextureCompression",1,0,4,1, {0,1,2,3,4},5},
    {"RenderQualityPerformance",1,0,7,1, {},0},
    {"RenderVKUpscaler",1,0,11,1, {0,1,2,3,4,5,6,7,8,9,10,11},12},
    {"RenderVKOpaqueShadingRate",1,0,3,1, {0,1,2,3},4},
    {"RenderVKAlphaShadingRate",1,0,4,1, {0,1,2,4},4},
    {"RenderShadowDetail",1,0,4,1, {},0},
    {"RenderVKShadowResolution",1,0,8192,1, {},0},
    {"RenderReflectionProbeDetail",1,0,4,1, {},0},
    {"RenderReflectionProbeLevel",1,0,4,1, {},0},
    {"RenderFSAAType",1,0,16,1, {},0},
    {"SlipstreamRenderTonemapType",1,0,16,1, {},0},
    {"RenderVKNoVsyncPresentMode",1,0,4,1, {},0},
    {"TuningFPSStrategy",1,0,8,1, {},0},
    {"RenderMaxTextureResolution",1,0,8192,1, {},0},
    {"RenderReflectionProbeResolution",1,0,2048,1, {},0},
    {"RenderReflectionProbeCount",1,0,256,1, {},0},
    {"RenderVKPipelineCompileThreads",1,0,128,1, {},0},
    {"RenderAvatarComplexityMode",1,0,8,1, {},0},
    {"RenderFarClip",1,64,512,1, {64,96,128,192,256,384,512},7},
    {"RenderFPSLimit",1,0,360,1, {0,30,50,60,75,90,100,120,144,165,180,200,240,360},14},
    {"TargetFPS",1,0,360,1, {0,30,50,60,75,90,100,120,144,165,180,200,240,360},14},
    {"RenderAvatarMaxNonImpostors",1,0,256,1, {0,1,2,4,8,16,32,64,128,256},10},
    {"RenderMaxVideoMemoryMB",1,0,65536,1, {0,512,1024,2048,3072,4096,6144,8192,12288,16384,24576,32768,49152,65536},14},
    {"ThrottleBandwidthKBPS",1,0,8192,1, {0,128,256,512,1024,2048,4096,8192},8},
    {"RenderVKRenderScale",2,0.25,2,0.05, {},0},
    {"RenderVolumeLODFactor",2,0,8,0.25, {},0},
    {"UIScaleFactor",2,0.5,4,0.25, {},0},
};
inline constexpr unsigned SETTING_COUNT = sizeof(SETTING_RULES)/sizeof(SETTING_RULES[0]);
}
