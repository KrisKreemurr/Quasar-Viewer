#version 460
#extension GL_GOOGLE_include_directive : require
// Pull only the indices and the EXISTING welded character-space skin output.
#define CLUSTER_SET 2
#define CL_DECLARE_POOL
#define CL_DECLARE_INSTANCES
#include "clean_cluster_common.glsl"
#include "clean_cluster_select_common.glsl"
#define CL_DECLARE_SKIN_OUT
#define CL_DECLARE_SKIN_SCRATCH
#include "clean_cluster_skin_common.glsl"
#include "clean_skinvert.glsl"
#include "clean_draw_view.glsl"
layout(location=12) flat out uint v_inst;
invariant gl_Position;
precise gl_Position;
void main()
{
    uint encoded=uint(gl_InstanceIndex),kind=encoded>>30u,sel=encoded&0x3fffffffu;
    gl_Position=vec4(0.0,0.0,-2.0,1.0);v_inst=0u;
    if(g_draw_push.f8<3u && kind!=g_draw_push.f8)return;
    ClSelected cs=cl_sel[sel];ClInstance ci=cl_instances[cs.instance];
    uint s=clSkinnedCluster(sel);if(s==CL_NONE)return;
    uint ib=uint(gl_VertexIndex),local=(cl_pool[ib>>2u]>>((ib&3u)*8u))&255u;
    if(local>=CL_MAX_CLUSTER_VERTS)return;
    uint v=s*CL_MAX_CLUSTER_VERTS+local;
    ClChar ch=clSkinChar(clSkinRow(ci.flags),false);
    vec3 rel=ch.m*SV_POS(cl_skin_out,v)+ch.t;
    gl_Position=pc.u_view_proj*vec4(rel+(clSkinEye()-pc.u_origin.xyz),1.0);
    v_inst=ci.render_instance;
}
