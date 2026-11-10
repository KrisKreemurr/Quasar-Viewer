// Previous SHADING normal from the same interpolated vertex, palette/root frame,
// normal texture and material UV transform. Current texture values are transported
// through the previous TBN: material changes still disagree with stored old guides.
#ifndef CLEAN_RECEIVER_NORMAL_GLSL
#define CLEAN_RECEIVER_NORMAL_GLSL
layout(location = 20) in vec3 v_prev_world_normal;
layout(location = 21) in vec4 v_prev_world_tangent;
vec3 receiverPreviousShadingNormal(Material m, vec4 previousClip)
{
    if (v_receiver_class != 2u) return vec3(0.0);
    if (!(dot(v_prev_world_normal,v_prev_world_normal)>1e-12) || !(previousClip.w>1e-6)
        || noTemporalFace(v_flags)) return vec3(0.0);
    // Local-DN guides store raster-facing shading normals, NOT GI's eye-facing
    // hemisphere. Transport the TRIANGLE winding through the previous projection;
    // a smooth/normal-mapped N may legitimately lean away from the eye.
    const vec2 previousNdc = previousClip.xy / previousClip.w;
    const vec2 dx = dFdx(previousNdc), dy = dFdy(previousNdc);
    const float area = dx.x * dy.y - dx.y * dy.x;
    if (!(abs(area) > 1e-20) || isnan(area) || isinf(area)) return vec3(0.0);
    const bool previousFront = area > 0.0 ? gl_FrontFacing : !gl_FrontFacing;
    vec3 N = normalize(v_prev_world_normal);
    if (!previousFront) N = -N;
    vec3 tn;
    if ((v_flags & 32u) == 0u && legacyBumpCode(m.flags) != 0u)
    {
        if (legacyBumpTN(m,v_uv,taaLodBias(v_flags),tn))
            N = applyNormalMap(normalize(v_prev_world_normal),v_prev_world_tangent,v_prev_world_pos,v_uv,tn,previousFront);
    }
    else if ((v_flags & 32u) == 0u && (m.flags & 2u) != 0u)
    {
        vec2 uv = (v_flags & 1024u) != 0u ? v_uv : matMapUV(v_material_id,MAP_NORMAL,v_uv_raw,v_uv);
        tn = cleanNormalTex(texture(uTextures[nonuniformEXT(m.normal_handle.x)],uv,taaLodBias(v_flags)).xyz);
        N = applyNormalMap(normalize(v_prev_world_normal),v_prev_world_tangent,v_prev_world_pos,v_uv,tn,previousFront);
    }
    return N;
}
#endif
