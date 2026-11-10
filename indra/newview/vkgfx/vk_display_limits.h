#pragma once

namespace vkdisplay
{
// Keep synchronized with kDisplayReferenceWhiteMaxNits in the display GLSL
// helpers; the calibration source contract checks all three definitions.
inline constexpr float kDisplayReferenceWhiteMaxNits = 1000.f;
}
