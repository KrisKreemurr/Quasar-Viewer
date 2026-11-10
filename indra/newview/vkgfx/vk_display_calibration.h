// Display-reported luminance and user choices are independent of scene exposure.
#pragma once
#include "vk_display_limits.h"

#include <algorithm>
#include <cmath>

namespace vkdisplay
{
struct ReportedLuminance
{
    float referenceWhiteNits = 0.f;
    float peakNits = 0.f;
    const char* referenceSource = "unavailable";
    const char* peakSource = "unavailable";
};

struct LogicalBounds { int x, y, width, height; };
#if LL_LINUX
void queryKscreenLuminance(const LogicalBounds& bounds, ReportedLuminance& report);
#endif

struct CalibrationChoice
{
    float referenceWhiteNits;
    float peakNits;
    const char* referenceSource;
    const char* peakSource;
};

struct CalibrationMigration
{
    bool automaticWhite, automaticPeak;
};

inline CalibrationMigration migrateCalibration(bool migrated, bool whiteDefault, bool peakDefault,
                                                bool automaticWhite, bool automaticPeak) noexcept
{
    // A non-default legacy white might be a manual or OS-derived Windows value.
    // There is no provenance; preserve it until the user opts in.
    return migrated ? CalibrationMigration{automaticWhite, automaticPeak}
                    : CalibrationMigration{whiteDefault, peakDefault};
}

inline CalibrationChoice chooseCalibration(const ReportedLuminance& reported,
                                           bool automaticWhite, float manualWhite,
                                           bool automaticPeak, float manualPeak) noexcept
{
    const bool haveWhite = std::isfinite(reported.referenceWhiteNits) &&
                           reported.referenceWhiteNits >= 80.f && reported.referenceWhiteNits <= kDisplayReferenceWhiteMaxNits;
    const bool havePeak = std::isfinite(reported.peakNits) &&
                          reported.peakNits >= 80.f && reported.peakNits <= 10000.f;
    const float white = automaticWhite && haveWhite ? reported.referenceWhiteNits : manualWhite;
    const float peak = automaticPeak && havePeak ? reported.peakNits : manualPeak;
    const float safePeak = std::clamp(std::isfinite(peak) ? peak : 1000.f, 80.f, 10000.f);
    const float desiredWhite = std::clamp(std::isfinite(white) ? white : 203.f, 80.f, kDisplayReferenceWhiteMaxNits);
    const float safeWhite = std::min(desiredWhite, safePeak);
    return {safeWhite, safePeak,
            desiredWhite > safePeak ? "limited to display peak" :
            (automaticWhite ? (haveWhite ? reported.referenceSource : "saved value (automatic unavailable)") : "user override"),
            automaticPeak ? (havePeak ? reported.peakSource : "saved value (automatic unavailable)") : "user override"};
}
}
