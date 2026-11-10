#!/usr/bin/env python3
"""Execute the production alpha classifier and near-opaque rule for seam triage.
Usage: skin_seam_alpha_checks.py <external output directory>. No viewer/GPU.
"""
from pathlib import Path
import subprocess
import sys
root = Path(__file__).resolve().parents[3]
out = Path(sys.argv[1]).resolve(); out.mkdir(parents=True, exist_ok=True)
src = (root/'indra/newview/llvovolume.cpp').read_text()
def body(text, signature):
    start = text.index(signature); op = text.index('{', start); depth = 1; end = op+1
    while depth:
        if text[end] == '{': depth += 1
        elif text[end] == '}': depth -= 1
        end += 1
    return text[start:end]
enum = body(src, 'enum ECleanAutoMask') + ';'
classifier = body(src, 'static void classifyCleanFace(')
shader = (root/'indra/vkclean/shaders/clean_blend_depth_rule.glsl').read_text()
rule = body(shader, 'bool blendDepthKeeps(')
cpp = r'''
#include <cassert>
#include <cstdint>
#include <iostream>
using U8=uint8_t;using U32=uint32_t;using S32=int32_t;using uint=uint32_t;
'''+enum+r'''
struct LLColor4 {float mV[4]{1,1,1,1};};
struct LLMaterial {enum {DIFFUSE_ALPHA_MODE_NONE=0,DIFFUSE_ALPHA_MODE_BLEND=1,DIFFUSE_ALPHA_MODE_MASK=2}; U8 mode=2; U8 getDiffuseAlphaMode()const{return mode;}};
struct LLGLTFMaterial {enum{ALPHA_MODE_OPAQUE,ALPHA_MODE_BLEND,ALPHA_MODE_MASK};int mAlphaMode=0;LLColor4 mBaseColor;};
struct MaterialPtr{LLMaterial* value=nullptr;LLMaterial*get()const{return value;}};
struct LLTextureEntry{LLColor4 color;MaterialPtr mat;LLGLTFMaterial*gm=nullptr;
 const LLColor4&getColor()const{return color;}
 LLGLTFMaterial*getGLTFRenderMaterial()const{return gm;}
 MaterialPtr getMaterialParams()const{return mat;}};
struct LLViewerTexture{int components=4;bool strict=true;int getComponents()const{return components;}bool getIsAlphaMask()const{return strict;}};
struct LLFace{LLTextureEntry*te=nullptr;LLViewerTexture*tex=nullptr;
 const LLTextureEntry*getTextureEntry()const{return te;}
 LLViewerTexture*getTexture()const{return tex;}
 bool canRenderAsMask()const{return false;}};
struct LLDrawPool{enum{POOL_ALPHA=1};};
struct LLPipeline{static int getPoolTypeFromTE(const LLTextureEntry*t,LLViewerTexture*){
 return t->mat.get()&&t->mat.get()->mode==LLMaterial::DIFFUSE_ALPHA_MODE_BLEND?LLDrawPool::POOL_ALPHA:0;}};
struct LLCleanTexture{static LLCleanTexture&instance(){static LLCleanTexture t;return t;}int alphaGradientClass(LLViewerTexture*){return 0;}};
struct Settings{}gSavedSettings;
template<class T>struct LLCachedControl{T value;LLCachedControl(Settings&,const char*,T x):value(x){}operator T()const{return value;}};
bool cleanFaceShowsMedia(LLFace*){return false;}
bool cleanEffectiveFullbright(LLFace*,const LLTextureEntry*){return false;}
bool cleanFaceIsRigged(LLFace*){return true;}
U8 cleanAutoMaskBlocker(LLFace*,const LLTextureEntry*){return CAM_NONE;}
'''+classifier+r'''
struct V{float z=0;};struct Frame{V ao_params2;}ssr;bool gl_FrontFacing=true;
constexpr uint REZFX_DYING=1u<<30;constexpr float kBlendDepthAlpha=.99f;
float rezfxFadeOpacity(uint){return 1;}
bool alphaKeeps(float a,float cutoff,float,float){return a>=cutoff;}
'''+rule+r'''
int main(){
 LLTextureEntry te;LLMaterial mat;LLViewerTexture tex;LLFace face{&te,&tex};te.mat.value=&mat;
 bool blend,mask,fullbright;U8 reason;
 // Reported authored MASK mode and zero tint alpha, even with a strictly maskable texture.
 te.color.mV[3]=0;
 classifyCleanFace(&face,blend,mask,fullbright,&reason);
 assert(CAM_BLEND_TINT==6&&blend&&!mask&&reason==6);
 assert(!blendDepthKeeps(0.f,.5f,256u,kBlendDepthAlpha));
 // Same MASK material with opaque tint: authored mask, not a failed promotion.
 te.color.mV[3]=1;
 classifyCleanFace(&face,blend,mask,fullbright,&reason);
 assert(!blend&&mask&&reason==CAM_NONE);
 // A genuinely BLEND-authored opaque-tint binary-alpha upper face can be promoted.
 mat.mode=LLMaterial::DIFFUSE_ALPHA_MODE_BLEND;
 classifyCleanFace(&face,blend,mask,fullbright,&reason);
 assert(!blend&&mask&&reason==CAM_STRICT);
 // Partial tint must remain translucent rather than become an opaque skin surface.
 te.color.mV[3]=.5f;
 classifyCleanFace(&face,blend,mask,fullbright,&reason);
 assert(blend&&!mask&&reason==CAM_BLEND_TINT);
 assert(!blendDepthKeeps(.5f,.5f,256u,kBlendDepthAlpha));
 std::cout<<"PASS actual classifier: MASK alpha0 stays blend/reason6; MASK alpha1 stays mask; BLEND alpha1 promotes; translucent texels stay outside near-opaque SSS\n";
}
'''
p=out/'checks.cpp';p.write_text(cpp)
subprocess.run(['c++','-std=c++20','-Wall','-Wextra','-Werror',str(p),'-o',str(out/'checks')],check=True)
subprocess.run([str(out/'checks')],check=True)
