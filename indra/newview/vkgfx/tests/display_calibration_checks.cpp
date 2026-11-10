#include "../vk_display_calibration.h"

#include <cassert>
#include <cstring>

int main()
{
    using namespace vkdisplay;
    const ReportedLuminance compositor{351.f, 409.f, "compositor reference", "output advertised peak"};
    const auto automatic = chooseCalibration(compositor, true, 200.f, true, 1000.f);
    assert(automatic.referenceWhiteNits == 351.f && automatic.peakNits == 409.f);
    assert(std::strcmp(automatic.referenceSource, "compositor reference") == 0);
    assert(std::strcmp(automatic.peakSource, "output advertised peak") == 0);

    const auto mixed = chooseCalibration(compositor, false, 200.f, true, 1000.f);
    assert(mixed.referenceWhiteNits == 200.f && mixed.peakNits == 409.f);
    assert(std::strcmp(mixed.referenceSource, "user override") == 0);
    const auto manual = chooseCalibration(compositor, false, 200.f, false, 1000.f);
    assert(manual.referenceWhiteNits == 200.f && manual.peakNits == 1000.f);
    const auto fallback = chooseCalibration({}, true, 200.f, true, 1000.f);
    assert(fallback.referenceWhiteNits == 200.f && fallback.peakNits == 1000.f);
    const auto constrained = chooseCalibration({351.f, 300.f, "reference", "peak"}, true, 200.f, true, 1000.f);
    assert(constrained.referenceWhiteNits == 300.f && constrained.peakNits == 300.f);
    assert(std::strcmp(constrained.referenceSource, "limited to display peak") == 0);
    const auto secondDisplay = chooseCalibration({455.f, 1000.f, "KScreen", "EDID"}, true, 203.f, true, 1000.f);
    assert(secondDisplay.referenceWhiteNits == 455.f && secondDisplay.peakNits == 1000.f);
    const auto fresh = migrateCalibration(false, true, true, false, false);
    assert(fresh.automaticWhite && fresh.automaticPeak);
    const auto customWhite = migrateCalibration(false, false, true, true, true);
    assert(!customWhite.automaticWhite && customWhite.automaticPeak);
    const auto customPeak = migrateCalibration(false, true, false, true, true);
    assert(customPeak.automaticWhite && !customPeak.automaticPeak);
    // OS-written and manual legacy white are indistinguishable: preserve either.
    const auto ambiguousWhite = migrateCalibration(false, false, true, true, true);
    assert(!ambiguousWhite.automaticWhite);
    const auto migrated = migrateCalibration(true, false, false, true, false);
    assert(migrated.automaticWhite && !migrated.automaticPeak);
}
