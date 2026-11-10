#!/usr/bin/env python3
"""Extract renderer arbitration/recovery code and verify progress using fake endpoints.

Use --source-root to validate a separate checkout without writing to it.
All generated C++, compiler output and receipts are written under --out.
"""
import argparse,hashlib,json,pathlib,re,resource,subprocess,sys,time
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source-root',type=pathlib.Path)
parser.add_argument('--out',type=pathlib.Path,required=True)
args=parser.parse_args()
root=pathlib.Path(__file__).resolve().parents[3]
source_root=(args.source_root or root).resolve()
out=args.out.resolve();out.mkdir(parents=True,exist_ok=True)
p=source_root/'indra/newview/vkgfx/llcleanrenderervk.cpp';source=p.read_text();pieces={}
header=source_root/'indra/newview/vkgfx/llcleanrenderervk.h'
header_source=header.read_text()
floor_dim=re.search(r'kTexFloorDim\s*=\s*(\d+)',header_source).group(1)
def section(name,start,end,offset=0):
    i=source.index(start,offset); j=source.index(end,i)
    pieces[name]={'start':source.count('\n',0,i)+1,'end':source.count('\n',0,j)+1,'sha256':hashlib.sha256(source[i:j].encode()).hexdigest()}
    return source[i:j]
def body(marker):
    i=source.index(marker); start=source.index('{',i);depth=1;j=start+1
    while depth:
        depth += (source[j]=='{')-(source[j]=='}');j+=1
    pieces[marker]={'start':source.count('\n',0,start)+1,'end':source.count('\n',0,j)+1,'sha256':hashlib.sha256(source[start:j].encode()).hexdigest()}
    return source[start:j]
active=section('active-arbitration','    auto sameRecipe =', '    uint32_t freeTickets =',source.index('LLCleanRendererVK::TexStageStartResult LLCleanRendererVK::startImmutableTexStage('))
cancel=section('cancel','void LLCleanRendererVK::cancelTexStage(uint64_t key)','\nuint32_t LLCleanRendererVK::uploadRealMaterialTextures').replace('LLCleanRendererVK::','Harness::')
cov=section('coverage-recipe','                        LLVKTexturePrep::Recipe recipe;', '                        (void)startImmutableTexStage',source.index('const bool cov_stale ='))
reenc=section('reencode-recipe','                    const uint32_t w = (uint32_t)raw->getWidth(), h = (uint32_t)raw->getHeight();','                    (void)startImmutableTexStage',source.index('const bool keep_cov_mips ='))
demand=body('auto texStageDemand =')
hint=re.search(r'uint32_t gpu_hint = (.*?);',source).group(0)
nominate=re.search(r'else if (\(\((?:!st.image \|\| )?T > R.*?\) <= 0\))',source).group(1)
reject=re.search(r'if (\((?:st.image && \()?nd < rd .*?\))\n',source).group(1)
needs_decode=re.search(r'ft->setVKDemand\(lld, mTexPass, gpu_hint, t2_safe, (.*?), st.mask_cut != 0\);',source).group(1)
coverage_guard=re.search(r'else if \((cov_starts < kCovStartsPerPass.*?)\)\n',source).group(1)
shared=body('auto sharedStagePending =') if 'auto sharedStagePending =' in source else '{ (void)key; return false; }'
reencode_guard=re.search(r'if \(sharedStagePending\(rk\)\).*?continue; \}',source)
reencode_guard=reencode_guard.group(0) if reencode_guard else ''
settling=section('settling','            const int32_t r[4] = { bi, ni, mi, ei };','            mMatSettled[i] =')
cpp=r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>
#include "llvktextureprep.h"
using U32=uint32_t;using VkFormat=uint32_t;
constexpr uint32_t VK_NULL_HANDLE=0, VK_FORMAT_R8G8B8A8_UNORM=37,VK_FORMAT_R8G8B8A8_SRGB=43,TEXENC_NONE=0,TEXENC_COUNT=6,VK_FORMAT_UNDEFINED=0;
struct Raw {uint32_t size=512; uint32_t getWidth()const{return size;}uint32_t getHeight()const{return size;}uint32_t getComponents()const{return 4;}};
struct SharedTex {uint32_t id=7,image=10,w=64,h=64,slot[2]={1,0},hold=512,up_wait_until=0,src_w=512,src_h=512,const_w=0,const_h=0,full_w=512,full_h=512,fmt=37;bool pinned=false,hud=false,bake=false,base_srgb=false,lossy=false,reenc_force=true;uint8_t mask_cut=100,cov=0,comps=4,enc=0,enc_level=4;};
struct TexEncPick {uint8_t enc=0;};
struct TexStageTicket {
 enum class Life {Free,Worker,Ready,GpuQueued,Recorded,Accepted,Quarantine};Life life=Life::Free;
 uint64_t key=0,serial=1;int generation=1;bool mediaTarget=false,syntheticInput=false,cancelWhenReady=false;
 uint32_t textureId=7,expectedOld=10,width=0,height=0,sourceWidth=512,sourceHeight=512,sourceFullWidth=512,sourceFullHeight=512,sourceComps=4,sourceEncoding=0,sourceEncodingLevel=4,sourceCoverageCut=0,destinationFormat=37;
 LLVKTexturePrep::SourceKind prepKind=LLVKTexturePrep::SourceKind::PixelBytes;LLVKTexturePrep::Recipe prepRecipe;
};
enum class TexStageStartResult {Accepted,Deferred};enum class TexStageDemand {Wanted,Target,Extent,Recipe,Covered,Policy};
uint32_t texMipLevels(uint32_t w,uint32_t h){uint32_t l=1;for(uint32_t n=std::max(w,h);n>1;n>>=1)++l;return l;}
VkFormat texEncFormat(uint8_t,bool){return 37;}
struct LLUUID {uint32_t value=0;bool isNull()const{return !value;}bool notNull()const{return value;}};
struct LLViewerTextureManager {static void* findMediaTexture(const LLUUID&){return nullptr;}};
struct Harness {
 bool cov_on=true;uint32_t mTexPass=2,mTexCompLevel=4;unsigned admissions=0,cancellations=0;
 std::array<TexStageTicket,1> mTexStageTickets;struct {unsigned cancelReplaced=0,cancelled=0;}mTexStageInterval;
 bool covFlat(const SharedTex& st)const COVFLAT
 uint8_t covWant(const SharedTex& st)const COVWANT
 bool covStale(const SharedTex& st)const COVSTALE
 bool sharedStagePending(uint64_t key)const SHARED
 static TexEncPick texEncFor(const SharedTex&,uint32_t,uint32_t){return {};}
 static bool texEncAcceptable(const SharedTex&){return true;}
 static bool texConstCollapse(const SharedTex&,uint32_t,uint32_t){return false;}
 static uint32_t texBias(uint32_t){return 0;}
 static uint32_t targetSkip(uint32_t sw,uint32_t sh,uint32_t demand,uint32_t bias) TARGETSKIP
 void cancelTexStage(uint64_t key);
 void releaseTexStageService(TexStageTicket&,bool cancel){if(cancel)++cancellations;}
 void releaseTexStageRetire(TexStageTicket&){}
 void discardTexStageCandidate(TexStageTicket&){}
 TexStageStartResult start(uint64_t key,const SharedTex& current,const LLVKTexturePrep::Recipe& requestedRecipe){
  const uint64_t sourceSerial=1;const int sourceGeneration=1;const auto recipe=requestedRecipe;
 ACTIVE
  for(auto& t:mTexStageTickets)if(t.life==TexStageTicket::Life::Free){t={};t.life=TexStageTicket::Life::Worker;t.key=key;t.prepRecipe=recipe;t.width=recipe.targetWidth;t.height=recipe.targetHeight;t.expectedOld=current.image;t.textureId=current.id;++admissions;return TexStageStartResult::Accepted;}
  return TexStageStartResult::Deferred;
 }
 LLVKTexturePrep::Recipe coverage(SharedTex& st){COVRECIPE return recipe;}
 LLVKTexturePrep::Recipe reencode(SharedTex& st,const Raw* raw){REENCODE return recipe;}
 TexStageDemand validate(const SharedTex& st,const TexStageTicket& ticket) DEMAND
 uint32_t gpuHint(const SharedTex& st){const bool cov_stale=covStale(st);HINT return gpu_hint;}
 bool needsDecode(const SharedTex& st,uint32_t gpu_hint,uint32_t need){(void)st;return NEEDSDECODE;}
 bool settled(uint32_t image){
  const int32_t bi=1,ni=-1,mi=-1,ei=-1;
  const LLUUID ids[4]={{7},{0},{0},{0}};const bool missing[4]={false,false,false,false};
  struct Image {uint32_t image;};const std::array<Image,2> mTextures={Image{0},Image{image}};
  (void)mTextures;
  SETTLING
  return settled;
 }
 bool offerCoverage(SharedTex& st){
  const unsigned cov_starts=0,kCovStartsPerPass=4;struct {uint64_t first=7;}kv;
  (void)kv;
  if(covStale(st)&&covWant(st)&&(COVERAGEGUARD)){start(kv.first,st,coverage(st));return true;}return false;
 }
 bool offerReencode(SharedTex& st,const Raw* raw){
  const uint64_t rk=7;std::vector<uint64_t>keep;
  for(int once=0;once<1;++once){REENCODEGUARD start(rk,st,reencode(st,raw));return true;}return false;
 }
 bool nominate(const SharedTex& st,uint32_t T){const uint32_t R=std::max(st.w,st.h);const bool collapse=false;return NOMINATE;}
 bool rejectsEqual(const SharedTex& st,uint32_t nd){const uint32_t rd=std::max(st.w,st.h);return REJECT;}
};
CANCEL
int main(){
 SharedTex st;Raw raw;Harness h;
 auto upgrade=h.coverage(st);upgrade.targetWidth=512;upgrade.targetHeight=512;upgrade.targetLevels=texMipLevels(512,512);
 assert(h.start(7,st,upgrade)==TexStageStartResult::Accepted);
 for(unsigned i=0;i<8;++i){
  assert(!h.offerCoverage(st));assert(!h.offerReencode(st,&raw));
  assert(!h.mTexStageTickets[0].cancelWhenReady);
 }
 assert(h.admissions==1&&h.cancellations==0);
 // Fake successful worker/upload endpoint; the real arbitration must leave it publishable.
 st.w=h.mTexStageTickets[0].width;st.h=h.mTexStageTickets[0].height;h.mTexStageTickets[0]={};
 assert(st.w==512);std::cout<<"PASS coverage/reencode preserve active upgrade through eight scans, publish 512\n";
 st.w=st.h=64;st.bake=true;assert(h.covWant(st)==0&&!h.covStale(st));assert(!h.offerCoverage(st));
 auto up=h.reencode(st,&raw);assert(up.targetWidth==64);up.targetWidth=up.targetHeight=512;up.kind=LLVKTexturePrep::RecipeKind::ConvertPixels;
 assert(h.start(7,st,up)==TexStageStartResult::Accepted);assert(!h.offerReencode(st,&raw));
 assert(!h.mTexStageTickets[0].cancelWhenReady);
 assert(h.start(7,st,up)==TexStageStartResult::Accepted);assert(h.admissions==2);
 std::cout<<"PASS server-bake coverage exclusion, reencode guard and identical-recipe control\n";
 TexStageTicket partial=h.mTexStageTickets[0];partial.width=partial.height=128;partial.sourceWidth=partial.sourceHeight=256;
 partial.prepRecipe.targetWidth=partial.prepRecipe.targetHeight=128;partial.prepRecipe.kind=LLVKTexturePrep::RecipeKind::ReducePixels;
 st.reenc_force=false;st.hold=128;assert(h.validate(st,partial)==TexStageDemand::Wanted);
 st.hold=512;assert(h.validate(st,partial)==TexStageDemand::Wanted);
 st.w=st.h=256;assert(h.validate(st,partial)==TexStageDemand::Extent); // regressing
 st.w=st.h=64;st.hold=64;assert(h.validate(st,partial)==TexStageDemand::Extent); // over demand
 st.hold=512;auto malformed=partial;malformed.prepRecipe.targetWidth=64;assert(h.validate(st,malformed)==TexStageDemand::Recipe);
 std::cout<<"PASS monotonic intermediate detail; regression, overshoot and malformed recipe rejected\n";
 st.image=0;st.w=st.h=512;assert(h.gpuHint(st)==0);assert(h.needsDecode(st,0,512));
 assert(h.needsDecode(st,1024,512)); // absent image still needs decode even with a cache hint
 assert(h.nominate(st,512));assert(!h.rejectsEqual(st,512));assert(!h.settled(0));assert(h.settled(10));
 std::cout<<"PASS NULL reserved extent: no fake GPU pixels, decode required, equal-size recovery admitted, NULL material unsettled\n";
 st.image=10;assert(h.gpuHint(st)==512);assert(!h.needsDecode(st,512,512));assert(!h.nominate(st,512));assert(h.rejectsEqual(st,512));
 std::cout<<"PASS resident-image control: covered pixels stay settled and skip redundant repair\n";
}

'''
replacements={'COVFLAT':body('auto covFlat ='),'COVWANT':body('auto covWant ='),'COVSTALE':body('auto covStale ='),'TARGETSKIP':body('uint32_t LLCleanRendererVK::texTargetSkip(').replace('kTexFloorDim',floor_dim+'u'),'ACTIVE':active,'COVRECIPE':cov,'REENCODE':reenc,'DEMAND':demand,'HINT':hint,'NOMINATE':nominate,'REJECT':reject,'CANCEL':cancel,'SHARED':shared,'NEEDSDECODE':needs_decode,'COVERAGEGUARD':coverage_guard,'REENCODEGUARD':reencode_guard,'SETTLING':settling}
for key in sorted(replacements,key=len,reverse=True):cpp=cpp.replace(key,replacements[key])
(out/'fixture.cpp').write_text(cpp)
(out/'source-manifest.json').write_text(json.dumps({'source':str(p),'source_sha256':hashlib.sha256(source.encode()).hexdigest(),'header':str(header),'header_sha256':hashlib.sha256(header_source.encode()).hexdigest(),'contract_root':str(root),'slices':pieces,'limits':['admission and worker-completion endpoints faked','no GPU, descriptor/fence or live avatar validation','positive regression; no live avatar validation']},indent=2)+'\n')
command=['/usr/bin/c++','-std=c++20','-Wall','-Wextra','-Werror','-I'+str(root/'indra/newview/vkgfx'),'-I'+str(root/'indra/llrender'),str(out/'fixture.cpp'),'-o',str(out/'fixture')]
(out/'command.json').write_text(json.dumps(command,indent=2)+'\n')
started=time.monotonic()
r=subprocess.run(command,capture_output=True,text=True,cwd=out);(out/'compile.log').write_text(r.stdout+r.stderr)
if r.returncode:print(r.stderr);sys.exit(r.returncode)
r=subprocess.run([str(out/'fixture')],capture_output=True,text=True,cwd=out)
(out/'run.log').write_text(r.stdout+r.stderr)
(out/'result.json').write_text(json.dumps({'exit_code':r.returncode,'elapsed_seconds':time.monotonic()-started,'source_sha256':hashlib.sha256(source.encode()).hexdigest()},indent=2)+'\n')
print(r.stdout+r.stderr);sys.exit(r.returncode)
