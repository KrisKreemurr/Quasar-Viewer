#include "../vk_display_calibration_kde.h"
#include <cassert>
#include <cmath>
#include <string>
#include <vector>

static void checksum(std::vector<uint8_t>& edid, size_t block)
{
    unsigned sum = 0;
    for (size_t i = 0; i < 127; ++i) sum += edid[block * 128 + i];
    edid[block * 128 + 127] = static_cast<uint8_t>(-sum);
}
int main()
{
    using namespace vkdisplay;
    const std::string output = R"({"connected":true,"enabled":true,"hdr":true,"name":"DP-2","pos":{"x":1728,"y":674},"size":{"width":3840,"height":2160},"scale":1.25,"rotation":2,"sdr-brightness":351})";
    const std::string json = "{\"outputs\":[" + output + "]}";
    KscreenMatch m;
    assert(matchKscreenOutput(json, {1728, 674, 3072, 1728}, m));
    assert(m.connector == "DP-2" && m.whiteNits == 351.f);
    assert(!matchKscreenOutput(json, {1728, 674, 3072, 1727 - 2}, m));
    assert(!matchKscreenOutput("{\"outputs\":[" + output + "," + output + "]}", {1728,674,3072,1728}, m));
    const std::string second = R"({"outputs":[{"connected":true,"enabled":true,"hdr":true,"name":"HDMI-A-1","pos":{"x":0,"y":0},"size":{"width":2560,"height":1440},"scale":1,"rotation":1,"sdr-brightness":455}]})";
    assert(matchKscreenOutput(second, {0,0,2560,1440}, m) && m.whiteNits == 455.f);
    const std::string portrait = R"({"outputs":[{"connected":true,"enabled":true,"hdr":true,"name":"DP-3","pos":{"x":0,"y":0},"size":{"width":2160,"height":3840},"scale":1.25,"rotation":2,"sdr-brightness":455}]})";
    assert(matchKscreenOutput(portrait, {0,0,1728,3072}, m) && m.whiteNits == 455.f);
    const std::string overlap = R"({"connected":true,"enabled":true,"hdr":false,"name":"DP-4","pos":{"x":2000,"y":700},"size":{"width":1000,"height":1000},"scale":1})";
    assert(!matchKscreenOutput("{\"outputs\":[" + output + "," + overlap + "]}", {1728,674,3072,1728}, m));
    assert(!matchKscreenOutput("{\"outputs\":[" + output + ",{\"connected\":true,\"enabled\":true}]}", {1728,674,3072,1728}, m));
    const std::string unsafe = R"({"outputs":[{"connected":true,"enabled":true,"hdr":true,"name":"../DP-2","pos":{"x":1728,"y":674},"size":{"width":3840,"height":2160},"scale":1.25,"sdr-brightness":351}]})";
    assert(!matchKscreenOutput(unsafe, {1728,674,3072,1728}, m));
    assert(!matchKscreenOutput("{bad", {1728,674,3072,1728}, m));
    std::vector<uint8_t> edid(384, 0);
    const uint8_t header[] = {0,255,255,255,255,255,255,0};
    for (int i = 0; i < 8; ++i) edid[i] = header[i];
    edid[126] = 2; checksum(edid, 0);
    edid[128] = 2; edid[129] = 3; edid[130] = 9;
    edid[132] = 0xe4; edid[133] = 6; edid[134] = 4; edid[135] = 1; edid[136] = 97;
    checksum(edid, 1); checksum(edid, 2);
    float peak = 0.f;
    assert(ctaAdvertisedMaximum(edid, peak));
    assert(std::abs(peak - 408.758859f) < .01f);
    auto corrupt = edid; corrupt[136] ^= 1; assert(!ctaAdvertisedMaximum(corrupt, peak));
    corrupt = edid; corrupt.pop_back(); assert(!ctaAdvertisedMaximum(corrupt, peak));
    corrupt = edid; corrupt[130] = 5; checksum(corrupt, 1); assert(!ctaAdvertisedMaximum(corrupt, peak));
    corrupt = edid; corrupt[134] = 0; checksum(corrupt, 1); assert(!ctaAdvertisedMaximum(corrupt, peak));
    corrupt = edid; corrupt[135] = 0; checksum(corrupt, 1); assert(!ctaAdvertisedMaximum(corrupt, peak));
    corrupt = edid; corrupt[129] = 2; checksum(corrupt, 1); assert(!ctaAdvertisedMaximum(corrupt, peak));
}
