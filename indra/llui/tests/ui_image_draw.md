# #1405 shared UI image fallback

The unified texture-preparation route is asynchronous. `resolveTexture()` returns
zero while a UI texture has no published resident; binding zero samples the solid
white texture. Widget-local button/editor fallbacks did not cover window/panel
backgrounds, inventory icons, list cells, or other state sprites.

The three shared textured 2D helpers now prepare before issuing geometry. A live
view owns a lazy history of at most 64 sites, keyed by local rectangle, current UI
translation, helper family, and same-rectangle occurrence. Each site remembers its
last drawable texture and sampling parameters (UV rectangle, nine-slice center,
rotation, solid/scale flags). Pending replacements draw that previous image if it
is still drawable. Without a drawable predecessor they emit no geometry. Deliberate
untextured rectangles are unchanged. Nested nine-slice calls share the outer
selection, rather than selecting a different fallback for individual slices.

History follows normal child drawing and all retained-floater capture, live, and
replay-live paths. It is owned by the view, so destroying a view releases references
and cannot alias a new view at the same address. Overflow evicts old sites that were
not used in the current draw; when all 64 are already used, ready content still
draws and pending content draws nothing. Allocation failure follows that same rule.
These references do not pin Vulkan residents or defeat the existing UI VRAM budget.
A pending desired image still requests preparation and invalidates retained capture,
so a recorded fallback cannot prevent the new state from eventually appearing.

The per-button/editor last-image caches and selection calls are removed. The old
selection-policy unit test remains as a compatibility test; production drawing uses
the shared helpers instead.

## Coverage checked from call sites

- Buttons, line editors, checkboxes and combo-box buttons: shared LLUIImage draws.
- Panel/window backgrounds, including Conversations/chat/IM containers: LLPanel's
  background LLUIImage draws, with retained floater paths scoped as well.
- Tabs and their arrow buttons: LLUIImage/button draws.
- Scroll-list icons, selection/highlight images and embedded controls: shared
  LLUIImage and child-control draws.
- Inventory folder/item/overlay/favorite icons and selection backgrounds:
  LLUIImage draws; raw rotated folder arrows also reach the shared helper.
- Menus: image-bearing controls use the same helpers; intentional solid backgrounds
  and text rendering are not turned into texture placeholders.

This is source and CPU coverage, not a claim of in-world visual verification.

## Relation to PR #1414

PR head `83c662aaad8449c5cf1e0647cd2c78398abdfe8a` was fetched and read.
Its small-sprite/font idle-retention policy is complementary: it avoids unnecessary
re-preparation. This fix handles asynchronous first use, VKUI_ICON/thumbnails, and
budget/pressure eviction without exempting more textures from the memory policy.
It does not modify `tickCache()` or the contributor's branch. The PR patch applies
cleanly to this tree and can land first or after this change.

## Checks (no viewer/GPU)

From the source root, with an owned disk output directory:

```
python3 indra/llui/tests/ui_image_draw_checks.py --out <owned-output>
python3 indra/llui/tests/widget_image_source_checks.py
c++ -std=c++20 -Wall -Wextra -Werror indra/llui/tests/widget_image_selection_test.cpp -o <owned-output>/old-selection
<owned-output>/old-selection
```

The first test extracts the actual production selector, scope implementation, draw
history declarations, and all three public wrappers. CPU stubs replace texture/VK
services only. It checks pending/ready transitions, exact old UV/nine-slice data,
view/layer/list-cell separation, source-copy requests, retained retry notification,
first-use suppression, the 64-site bound, injected allocation failure, and actual
wrapper calls emitting fallback geometry or nothing. ASan/UBSan also passed with
leak checking disabled (the execution sandbox prevents LeakSanitizer under ptrace).

Actual configured Linux project commands were copied and retargeted to owned object
outputs, with `-Werror`: llrender2dutils, lluiimage, llview, llfloater, llbutton,
lllineeditor. No shared build directory was written and no viewer was launched.
