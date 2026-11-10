#version 450
// vkclean — edit-mode selection outline (LL renderSilhouettes' per-face wireframe).
// Drawn with polygonMode LINE over the selected instances; the colour (LL's
// silhouette parent/child colour, alpha dimmed for occluded edges) rides in the
// draw view's last vec4 (water_fog slot, unused by this pass).
#extension GL_GOOGLE_include_directive : require
layout(location = 0) out vec4 outColor;

#include "clean_draw_view.glsl"   // #238: the view block (pc.u_*) + 16 B push
#include "clean_frame_info.glsl"
layout(constant_id = 22) const bool kDisplayOverlayLinear = false;

void main()
{
    outColor = pc.u_water_fog;   // the selection colour
    if (kDisplayOverlayLinear && ssr.display_output.y > 0.5)
    {
        vec3 encoded = clamp(outColor.rgb, vec3(0.0), vec3(1.0));
        outColor.rgb = mix(pow((encoded + 0.055) / 1.055, vec3(2.4)),
                           encoded / 12.92, lessThanEqual(encoded, vec3(0.04045)))
                     * ssr.display_output.x;
    }
}
