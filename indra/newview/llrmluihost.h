// #703 RmlUi host: process lifecycle, the main-window context and its documents.
//
// UIRenderer (restart required, read once at the first frame):
//   0 = Classic, the LLView renderer (default while the RmlUi presenters grow);
//   1 = Classic on RmlUi: the current UI, presented through RmlUi where every view of a
//       floater has a presenter (llrmluiclassic.h), the LLView renderer elsewhere;
//   2 = New: the new interface (llnewui.h), its own documents over the classic LLView UI.
// RenderVKRmlUiDemo (session only) adds the glass proof document in any mode.
#pragma once
#include <cstdint>

void slRmlUiTick() noexcept;
// The renderer chosen for this session (UIRenderer at startup).
uint32_t slRmlUiMode() noexcept;
