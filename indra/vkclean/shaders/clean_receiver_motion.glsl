// Motion attachment RGBA16F: xy unjittered UV current-minus-previous;
// z previous clip W (= positive view depth for the perspective motion camera);
// w:0 unknown/clear,1 static,2 legacy skinned fallback,3 skinned invalid frame;
// 1024..2047 encodes a class-2 previous SHADING normal, oct5x5 (exact fp16 integers).
// XY/depth and the 8B allocation are unchanged. All class readers use receiverMotionClass.
#ifndef CLEAN_RECEIVER_MOTION_GLSL
#define CLEAN_RECEIVER_MOTION_GLSL
uint receiverMotionClass(float metadata)
{
    if (metadata == 2.0 || metadata == 3.0 || (metadata >= 1024.0 && metadata <= 2047.0)) return 2u;
    return metadata == 1.0 ? 1u : 0u;
}
bool receiverHasPreviousNormal(float metadata) { return metadata >= 1024.0 && metadata <= 2047.0; }
vec2 receiverOctWrap(vec2 v) { return (1.0-abs(v.yx))*vec2(v.x>=0.0?1.0:-1.0,v.y>=0.0?1.0:-1.0); }
vec3 receiverPreviousNormal(float metadata)
{
    uint bits = uint(metadata) - 1024u;
    vec2 e = vec2(float(bits & 31u), float(bits >> 5u)) * (2.0/31.0) - 1.0;
    vec3 n = vec3(e, 1.0-abs(e.x)-abs(e.y));
    if (n.z < 0.0) n.xy = receiverOctWrap(n.xy);
    return normalize(n);
}
// The raw oct decode has length >=1/sqrt(3). Quantizing each coordinate by
// <=1/31 changes it by <=sqrt(6)/31, even across folds. Angular error is
// <=2 asin(sqrt(18)/62)=.136967 radians. Shrink acos(.9) by that bound:
// cos(acos(.9)-error)=.951088. The rounded-up cutoff is conservative; a
// quantized match NEVER admits a true normal beyond the original acos(.9).
const float RECEIVER_PREVIOUS_NORMAL_CUT = 0.9511;
// Preserve each local-DN test's angular limit after quantizing the EXPECTED
// previous normal: shrink acos(cut) by the codec's proven worst-case error.
float receiverPreviousNormalCut(float cut)
{
    const float error = 0.136968;
    if (cut >= cos(error)) return 1.0;
    return min(cut * cos(error) + sqrt(max(1.0 - cut * cut, 0.0)) * sin(error), 1.0);
}
vec4 receiverMotionNormal(vec4 motion, vec3 previousShadingNormal)
{
    if (receiverMotionClass(motion.w) != 2u) return motion;
    float n2 = dot(previousShadingNormal, previousShadingNormal);
    if (!(n2 > 1e-12) || any(isnan(previousShadingNormal)) || any(isinf(previousShadingNormal)))
    { motion.w = 3.0; return motion; } // explicit invalid frame, not a normal-test bypass
    vec3 n = previousShadingNormal / (abs(previousShadingNormal.x)+abs(previousShadingNormal.y)+abs(previousShadingNormal.z));
    vec2 e = n.z >= 0.0 ? n.xy : receiverOctWrap(n.xy);
    uvec2 q = uvec2(clamp(round((e*.5+.5)*31.0), vec2(0.0), vec2(31.0)));
    motion.w = float(1024u + q.x + (q.y << 5u));
    return motion;
}
vec4 receiverMotionPayload(vec4 cur, vec4 prev, uint receiverClass)
{
    bool sane = cur.w > 1e-6 && prev.w > 1e-6 && prev.w <= 65504.0
        && !any(isnan(cur)) && !any(isinf(cur)) && !any(isnan(prev)) && !any(isinf(prev));
    vec2 delta = sane ? (cur.xy / cur.w - prev.xy / prev.w) * 0.5 : vec2(0.0);
    if(any(isnan(delta)) || any(isinf(delta)) || any(greaterThan(abs(delta),vec2(65504.0)))) sane=false;
    return vec4(sane?delta:vec2(0.0),sane?prev.w:0.0,float(receiverClass));
}
#endif
