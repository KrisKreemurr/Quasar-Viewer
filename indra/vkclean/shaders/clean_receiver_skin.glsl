// Previous pose: xyz plus compact N/T. Reserve a normal word the snorm16
// encoder cannot emit, so an invalid deformation never becomes its +Z fallback.
#ifndef CLEAN_RECEIVER_SKIN_GLSL
#define CLEAN_RECEIVER_SKIN_GLSL
#include "clean_skinvert.glsl"
uint receiverPackSkinnedNormal(vec3 n)
{
    float n2=dot(n,n);
    return n2>1e-20 && n2<3e38 && !any(isnan(n)) && !any(isinf(n)) ? svPackNormal(n) : 0x00008000u;
}
vec3 receiverUnpackSkinnedNormal(uint word)
{
    return word==0x00008000u ? vec3(0.0) : svUnpackNormal(word);
}
#endif
