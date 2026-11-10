# Rigged cluster overlays

Cluster-owned rigged rows intentionally have no legacy packed vertices, indices or
weights. `clean_sel_rig.comp` can only copy the main cull's legacy candidate; its
empty/culled candidate therefore makes an owned mesh's selection empty. Wireframe
had the same dependency on the legacy rigged buckets.

Selection (classic, wireframe and screen mask), global rigged wireframe and the
avatar ID mask used by cel outlines / FX now pull the current main-view cluster
cut and its completed welded SKIN_OUT. A compute filter emits non-indexed indirect
commands whose first vertex addresses the cluster's uint8 index bytes. The vertex
shader uses the character transform and eye/origin subtraction used by the normal
cluster draw. Neither missing maps nor allocation failure use a rest-pose or legacy
upload fallback. System bodies and other non-routable rows keep their legacy path.

The filter only runs for an actual selection, wireframe, or requested avatar-ID
mask. There are three lazy command buffers, each limited to 2 MiB actual Vulkan
allocation (6 MiB total). Each recording can emit at most 65,535 cluster commands;
extra commands are truncated by the indirect-count maximum. Selected face input
is capped at 8,192 rows. Vulkan-ordered input updates and count clears make repeated
snapshot recordings safe without CPU writes racing queued dispatches. Buffer
allocation failure omits that optional overlay and retries on a later recording.

## Consumer audit

- Classic, wireframe and screen-mask selection: new cluster route, including
  root/child/hover ordering and hidden/visible passes.
- Global wireframe: new cluster route. Does not request legacy retention.
- Hide Selection: main cluster draw and pick now read the existing bounded sorted
  hide-ID tail; selection still has the shared cut/pose needed to outline hidden rows.
- Pick/hover: existing cluster pick cut and pull draw retained; hidden rows excluded.
- Cel outline / avatar-ID FX mask: new cluster pull variant uses normal material UV,
  alpha-blend testing and avatar slot; system-body legacy draws remain.
- Snapshot outline / stereo overlay: same prepared commands, each recording's
  view and current cluster pose; no host count or input overwrite across recordings.
- Physics shapes: `llcleanphysshape.cpp` explicitly excludes worn rigged meshes.
  Animated-object shapes use their object pose row, not skinned vertices.
- Shadow, probe/mirror capture and RT: already have cluster rig lists / shared skin
  or derived RT cut. Their geometry and ray paths are unchanged.

## Checks and owner validation

`python3 indra/newview/tests/cluster_selection_checks.py <external-output-dir>`
executes the production legacy fixup and new filter bodies in a CPU API fixture:
legacy candidate present/off, absent/owned, current-cut command and kind, missing
skin map, policy/self hiding, wireframe, indirect cap and hide-ID lookup. It is a
source/command regression check, not a raster or Vulkan lifetime proof.

Regenerate shaders using `indra/vkclean/regen_world_spv.py` into the build tree;
never commit generated SPIR-V headers. The source task does not run the viewer/GPU.
Owner check: select the same worn attachment with CLOD off/on, all three selection
styles, hover, Hide Selection, global wireframe, then a snapshot including outlines.
Also check cel outlines/FX avatar mask on a cutout and blended attachment. Edges
must follow the animated pose and not reveal policy-hidden or first-person self.
