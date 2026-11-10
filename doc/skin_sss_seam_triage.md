# Skin seam: selected-face facts

The affected lower-leg face has not been identified in a local capture or numeric
face record. This patch supplies the requested diagnostic; it does not claim to
have fixed that face or expand skin classification speculatively.

Enable `RenderVKSkinFaceDiag` (Persist=0, default off). Use Edit > Select Face on
one lower-leg face that is black in RenderVKDebugView 31, then on one upper-body
face that scatters. Look for `VKSkinFace` lines. Static attachment signatures include
the selected diagnostic state, so selecting an unchanged attachment also produces
an ingest record. Turn the diagnostic off afterwards.

All logged fields are numeric/boolean: face and renderer row/material indices,
legacy and PBR base-colour bake slots, chosen slot, whether a bake was found,
PBR/legacy route, authored alpha mode, automatic alpha classification reason,
final blend/mask flags, tint alpha, HUD/fullbright exclusions, requested skin flag,
and the material store's actual packed skin bit and flags. There are no names,
UUIDs or paths. A fixed 128-entry cooldown table and 16 lines/s cap bound the
work/output; a face is normally logged at most once in five seconds. For large
selections use Select Face to isolate the relevant face instead of selecting an
entire linkset. There is no persistent selection history or new GPU work.

Bake indices: -1 not a bake; 0 head, 1 upper, 2 lower, 3 eyes, 4 skirt, 5 hair,
6 left arm, 7 left leg, 8/9/10 AUX1/2/3. Alpha mode is the authored PBR or legacy
numeric enum; final blend/mask booleans show the actual bucket after promotion.
`alpha_reason` uses the existing CleanAutoMaskReason enum in llvovolume.cpp.

## Source findings

- `cleanBomSkinBake` accepts head/upper/lower/left-arm/left-leg. Thus upper/lower
  alone cannot explain the absent lower-leg mask.
- The current gate excludes HUD/fullbright, not blend. A blended face retains the
  material tag, but only its G-buffer-admitted texels can scatter. A genuinely
  translucent forward-only face cannot be repaired by changing that tag alone.
- Both material hashing and equality include `skin`; packed bit23 is emitted by
  getOrCreate. There is no proven missing skin key in this path.
- The cluster draw uses the renderer row's material_id and the common draw fragment
  shader reads bit23 into SurfaceLit.skin. There is no separate CLOD fragment
  variant that simply omits the skin assignment.
- gbufferPack gives rez and cel variants priority over the skin variant. A face can
  have skin_desc=1 / skin_packed=1 yet be black in view31 for those reasons or
  because it is forward-only. The diagnostic plus views31/33 distinguishes this
  from an ingest exclusion; it does not guess the GPU variant from CPU facts.
- The SSS pass tests neighbouring pixels for skin and world-distance weighting,
  not instance/material/mesh ID. It already accepts a neighbouring skin face
  across a mesh seam. A black view31 neighbour is excluded before the gather.
- AUX slots are not intrinsically skin: the same slots can contain nails, teeth,
  lashes or body surfaces. No evidence currently supports opting all of them in.

Needed next: the two numeric face records and their view31/33 appearance. Then fix
that demonstrated exclusion or G-buffer ownership conflict, with a regression
case for it. Do not infer a classification fix from a CPU bake-table test alone.

Checks: actual llvovolume.cpp syntax with project flags/-Werror and normal project
warning exceptions; settings XML/default/Persist checks; existing source-bound
skin_sss_checks.py and skin_sss_debug_checks.py pass. No viewer/GPU run or visual
seam-continuity claim.

## Follow-up: build 0402f91aef numeric record

The returned face reports AUX1 (8), legacy material alpha mode MASK (2), but
TE tint alpha exactly 0, final blend=1, reason=6 and skin_desc=0. Those are two
independent facts:

1. AUX1 is excluded by the current semantic skin table.
2. CAM_BLEND_TINT (6) correctly preserves real tint transparency. An authored
   MASK mode does not override a zero-alpha tint. The classifier routes it to
   blend before the material-mask branch, exactly as its comments describe.

The renderer copies legacy TE alpha to instance tint. decodeSurface multiplies
that tint into base alpha. The near-opaque rule rejects this zero-coverage surface;
legacy glare is also multiplied by face alpha. Promoting it would not repair a
visible skin seam: either it would still discard, or forcing alpha to one would
reveal an intentionally hidden surface. With opaque tint, the same authored MASK
face stays masked (reason0); an authored BLEND face with opaque tint and binary
alpha can instead be promoted (reason2). Do not confuse these cases.

`skin_seam_alpha_checks.py <external-output-dir>` executes the production
classifyCleanFace function and blendDepthKeeps function with narrow API stubs.
It proves all four cases (zero, opaque MASK, promoted BLEND, partial tint), using
an otherwise promotion-eligible texture. It does not prove that this hidden face
is the one producing the visible lower-leg pixels.

The diagnostic now includes a bounded same-mesh census: mesh_faces/scanned,
mesh_bake_mask, mesh_skin_faces and mesh_hidden_faces (at most64 TEs per emitted
line). Bit n of mesh_bake_mask means bake n is present. This tests whether the
proposed canonical-bake sibling rule even applies to the affected mesh. It is
only evidence, not an automatic classification rule. A nail/teeth/lash AUX face
on that same mesh would pass a bare sibling test too; no renderer input labels
those arbitrary AUX images as anatomical skin. Separate objects versus shared
faces and the actual visible face therefore matter.

Next owner check: Select Face on the visible lower-leg surface, inspect its
Transparency in the editor, then collect that numeric line and the upper-body
line. If the reported alpha-zero face is deliberately hidden, leave it hidden.
Also confirm whether nails/teeth/lashes share this mesh. No speculative AUX
admission or alpha promotion is included in this diagnostic follow-up.

## Read-only diagnostic follow-up

The diagnostic no longer adds its enabled/selected state to a registration
signature. It observes the committed row even when ordinary ingest deduplicates
it. The prior instructions about forcing a diagnostic re-ingest are superseded.
Lines now include actual stored tint alpha, flags, skin ID, LOD group and current
source LOD. This containment is not a claim to fix the selected CLOD gap; see
select_gap_triage.md for the runtime evidence and remaining A/B.
