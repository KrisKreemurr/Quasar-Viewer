// Shared display transfer helpers for the live HDR output and SDR snapshot path.
// The input is the already tone-mapped display image in absolute Rec.709 nits.
const float kDisplayReferenceWhiteMaxNits = 1000.0; // vk_display_limits.h
vec3 displayNitsToSdrLinear(vec3 nits, float paperWhiteNits, float peakNits)
{
    float white = (isnan(paperWhiteNits) || isinf(paperWhiteNits)) ? 203.0 :
        clamp(paperWhiteNits, 80.0, kDisplayReferenceWhiteMaxNits);
    // Match vkdisplay::sdrSnapshotLinear: this input already passed the scene
    // tone curve. Applying a second shoulder dims SDR UI white to roughly 0.5.
    // SDR copies retain the reference-white range; HDR-only values clip here.
    // peakNits is kept in the shared call contract for display-output callers.
    for (int channel = 0; channel < 3; ++channel)
        if (isnan(nits[channel]) || isinf(nits[channel])) nits[channel] = 0.0;
    return clamp(nits, vec3(0.0), vec3(white)) / white;
}

vec3 displayLinearToSrgb(vec3 linearValue)
{
    linearValue = clamp(linearValue, 0.0, 1.0);
    return mix(1.055 * pow(linearValue, vec3(1.0 / 2.4)) - 0.055,
               12.92 * linearValue, lessThanEqual(linearValue, vec3(0.0031308)));
}
