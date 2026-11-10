// Continuous world-space weather noise, shared by baking and density warp.
#ifndef CLEAN_VOL_CLOUD_FIELD_GLSL
#define CLEAN_VOL_CLOUD_FIELD_GLSL
uint wxHash(ivec2 cell)
{
    uint h=uint(cell.x)*1597334677u+uint(cell.y)*3812015801u;
    h=(h^(h>>16u))*2246822519u;
    h=(h^(h>>13u))*3266489917u;
    return h^(h>>16u);
}
float wxValue(vec2 p)
{
    const ivec2 cell=ivec2(floor(p));
    const vec2 f=fract(p),w=f*f*(3.0-2.0*f);
    return mix(mix(float(wxHash(cell)),float(wxHash(cell+ivec2(1,0))),w.x),
               mix(float(wxHash(cell+ivec2(0,1))),float(wxHash(cell+ivec2(1,1))),w.x),w.y)*(1.0/4294967295.0);
}
#endif
