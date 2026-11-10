#!/usr/bin/env python3
"""Run the actual skin diagnostic observer with read-only API stubs.
No renderer, viewer or GPU. Usage: script <external-output-dir>.
"""
from pathlib import Path
import subprocess
import sys
root=Path(__file__).resolve().parents[3]; out=Path(sys.argv[1]).resolve();out.mkdir(parents=True,exist_ok=True)
s=(root/'indra/newview/llvovolume.cpp').read_text()
a=s.index('static void cleanSkinFaceDiagObserve(');b=s.index('// AUTO-MASK COUNTER',a)
observer=s[a:b]
# Diagnostics must not enter the registration signature or perform publication.
assert 'skin_diag_selected' not in s
for prohibited in ['upsert(', 'getOrCreate(', 'markDirty(', 'registerCleanFace(']:
 assert prohibited not in observer, prohibited
cpp=r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <array>
using U32=uint32_t;using U8=uint8_t;using S32=int32_t;
struct LLUUID{};
struct LLViewerObject{U32 getID()const{return 42;}};
struct LLFace{};struct LLTextureEntry{};
struct LLCleanInstanceStore{
 struct Instance{std::array<float,16>model{};U32 flags=3,material_id=77,skin_id=8,lod_group_id=9;float tint[4]{1,1,1,1};};
 Instance stored;bool available=true;U32 reads=0;
 static LLCleanInstanceStore&instance(){static LLCleanInstanceStore s;return s;}
 bool debugGet(U32,S32,Instance&out,U32*id){++reads;if(!available)return false;out=stored;*id=3310;return true;}
};
struct LLCleanMaterialStore{
 struct Material{U32 flags=16;};struct MaterialDesc{bool skin=false,gltf=false,legacy=false;};
 bool available=true;U32 reads=0;
 static LLCleanMaterialStore&instance(){static LLCleanMaterialStore s;return s;}
 bool debugGet(U32,Material&out,LLUUID&){++reads;out.flags=16;return available;}
};
bool ready=false;U32 logs=0;constexpr U8 CAM_NONE=0;
bool cleanSkinFaceDiagReady(LLViewerObject*,S32){return ready;}
bool cleanFaceSkinBake(const LLTextureEntry*,U32){return false;}
void classifyCleanFace(LLFace*,bool&blend,bool&mask,bool&fullbright,U8*reason){blend=true;mask=false;fullbright=false;*reason=10;}
void cleanSkinFaceDiag(LLViewerObject*,S32,const LLTextureEntry*,const LLCleanMaterialStore::MaterialDesc&d,
 U32 flags,U32 material,U32 row,U8 reason,U32 skin,U32 group,float alpha){
 assert(flags==3&&material==77&&row==3310&&reason==10&&skin==8&&group==9&&alpha==1);
 assert(d.legacy&&!d.gltf&&!d.skin);++logs;
}
'''+observer+r'''
int main(){
 LLViewerObject o;LLFace f;LLTextureEntry te;auto&store=LLCleanInstanceStore::instance();auto&mat=LLCleanMaterialStore::instance();
 for(U32 i=0;i<16;++i)store.stored.model[i]=float(i)*.125f;
 const auto before=store.stored;
 cleanSkinFaceDiagObserve(&o,2,&f,&te);assert(store.reads==0&&logs==0);
 for(U32 i=0;i<100;++i){ready=(i%2)==0;cleanSkinFaceDiagObserve(&o,2,&f,&te);assert(std::memcmp(&before,&store.stored,sizeof(before))==0);}
 assert(logs==50&&store.reads==50&&mat.reads==50);
 ready=true;store.available=false;cleanSkinFaceDiagObserve(&o,2,&f,&te);assert(logs==50&&mat.reads==50);
 store.available=true;mat.available=false;cleanSkinFaceDiagObserve(&o,2,&f,&te);assert(logs==50);
 assert(std::memcmp(&before,&store.stored,sizeof(before))==0);
}
'''
p=out/'checks.cpp';p.write_text(cpp)
subprocess.run(['c++','-std=c++20','-Wall','-Wextra','-Werror',str(p),'-o',str(out/'checks')],check=True)
subprocess.run([str(out/'checks')],check=True)
print('PASS production observer: off has no row reads; toggles leave row/pose/skin/group untouched; absent row/material never publishes')
