// vkclean -- THE SIDE A RAY-TRACED HIT'S SHADING NORMAL IS ON. Shared by clean_rt_surface.glsl (rtSurfaceEval)
// and the CPU fixture scripts/check/test_rt_hit_side.cpp (scripts/check/checks/gi_hit_side.py), so the rule the
// check runs is the rule the GPU runs.
//
// The raster orients a fragment's normal by its triangle's WINDING alone (clean_draw.frag: gl_FrontFacing ? n :
// -n), and rtSurfaceEval did the same at every hit: s.N = front ? nv : -nv, nv the interpolated vertex normal.
// That puts the normal on the side the ray arrived from only when the mesh's vertex normals agree with its
// winding. SL content is full of meshes whose normals do not -- flipped-normal uploads, inside-out sculpts,
// double-sided cloth / leaves exported with one side's normals: there s.N points AWAY from the ray (into the far
// side of the surface) while s.Ng is the arrival side. A GI hit shaded like that is lit by the FAR side's
// lights: rtHitLocalLights takes N . l > 0 for a lamp BEHIND the sheet, and its shadow ray (rtrProjRaySetup:
// origin 1 cm along N turned toward the light) starts on the far side and never crosses the sheet -- the lamp
// lights the side the ray sees, unshadowed. A lamp right behind a thin layer makes a few hits extremely bright:
// the reservoirs prefer them and hand them on, the GI fireflies on the surface just behind a back-facing layer.
// The hit's ambient (the radiance cache cell's face bucket, the probes' irradiance) was read along the wrong
// normal too.
//
// THE RULE (arrival = true; the GI hits): the shading normal takes the facing that puts the VERTEX normal on the
// arrival side -- front when nv agrees with the winding normal ng, else !front. The GI receivers already orient
// theirs toward the eye (clean_deferred_light.comp: N = dot(s.N, V) < 0 ? -s.N : s.N), and the rigged hits fall
// back to the facet when their vertex normal is on the hidden side; this is the static hits' counterpart. The
// normal map's result is negated with it (rtSurfApplyNormalMap(side, ...)), as a receiver's flip negates its
// normal-mapped N. arrival = false (RT reflections, probe captures): the raster's rule, unchanged -- a reflected
// / captured hit keeps shading like the raster fragment it stands for.
#ifndef CLEAN_RT_HIT_SIDE_H
#define CLEAN_RT_HIT_SIDE_H
#ifdef __cplusplus
#define RHS_FN inline
#else
#define RHS_FN
#endif
// front: the raster's facing of the hit triangle (its winding normal ng faces the arriving ray); nv: the
// interpolated vertex normal, unflipped (world, any length). Returns the facing the shading normal takes:
// N = side ? nv : -nv (and rtSurfApplyNormalMap(side, ...)).
RHS_FN bool rtHitNormalFront(bool front, vec3 nv, vec3 ng, bool arrival)
{
    return (arrival && dot(nv, ng) < 0.0f) ? !front : front;
}
#endif
