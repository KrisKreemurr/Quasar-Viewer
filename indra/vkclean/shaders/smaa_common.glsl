// vkclean -- shared SMAA 1x setup for the smaa_*.{vert,frag} wrappers.
// Push constant: SMAA_RT_METRICS = (1/w, 1/h, w, h). The preset
// (SMAA_PRESET_LOW/MEDIUM/HIGH/ULTRA) comes from the regen script's -D.
layout(push_constant) uniform SmaaPush { vec4 rt; } smaa_pc;
#define SMAA_RT_METRICS smaa_pc.rt
#define SMAA_GLSL_4 1
#define SMAA_FLIP_Y 0
#define SMAA_PREDICATION 0
#define SMAA_REPROJECTION 0

// HDR is selected only for the live linear edge/neighbor fragment pipelines.
// Offscreen gamma-SDR recordings retain the original pipelines and transfer.
layout(constant_id=0) const bool kSmaaDisplayLinear=false;
vec3 smaaDisplayProxy(vec3 colour) {
    if(!kSmaaDisplayLinear) return colour;
    colour=max(colour,vec3(0.0));
    return sqrt(colour/(1.0+colour)); // bounded perceptual proxy in relative-white units
}
