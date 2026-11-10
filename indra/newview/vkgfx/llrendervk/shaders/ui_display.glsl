// Optional linear display-nits output. Specialization false removes all extra
// math and push loads from the existing SDR/RTT pipelines.
layout(constant_id=0) const bool kDisplayLinearNits=false;
layout(constant_id=1) const bool kDisplayPremultiplied=false;
layout(push_constant) uniform DisplayUi {
    layout(offset=84) float paperWhiteNits; // already reserved by the fragment push range
} displayUi;
const float kDisplayReferenceWhiteMaxNits = 1000.0; // vk_display_limits.h
vec4 displayUiColour(vec4 colour,float coverage) {
    if(!kDisplayLinearNits) return vec4(colour.rgb,colour.a*coverage);
    float alpha=clamp(colour.a,0.0,1.0);
    vec3 source=kDisplayPremultiplied ? (alpha>0.0 ? colour.rgb/alpha : vec3(0.0)) : colour.rgb;
    source=clamp(source,0.0,1.0);
    vec3 linearValue=mix(pow((source+.055)/1.055,vec3(2.4)),source/12.92,
                         lessThanEqual(source,vec3(.04045)));
    float whiteNits=displayUi.paperWhiteNits;
    if(isnan(whiteNits)||isinf(whiteNits)) whiteNits=203.0;
    linearValue*=clamp(whiteNits,80.0,kDisplayReferenceWhiteMaxNits);
    if(kDisplayPremultiplied) linearValue*=alpha*coverage;
    return vec4(linearValue,alpha*coverage);
}
