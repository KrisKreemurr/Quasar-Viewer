# Selected CLOD face gap: current evidence

The owner reports row3310/face2 stays misaligned after deselection and after a
CLOD off/on cycle; the legacy path aligns it. A root-cause fix is not yet proven.
Do not label the diagnostic containment below as that pose fix.

The private live log was copied into the owned .validation directory with0600
permissions. No private log, identity or asset UUID belongs in this commit.

## Confirmed

- The CLOD module really was released at00:18:31Z and recreated at00:19:11Z.
  Existing SKIN_OUT and per-module candidate/bind caches therefore cannot alone
  explain a gap that returns after that recreation.
- Around the report, all reported source meshes were LOD3 (1411/0/0/0), with no
  source upgrades. This is evidence against a low-creator-LOD rebuild explanation.
- Sampled weld frames reported zero cached pairs and fresh searches, with zero
  invalid-pair count and no alpha-list overflow. The source audit identified
  possible overflow/self-heal edge defects, but the log does not establish either
  as the cause here. They have not been bundled into this patch.
- Row3310 was authored MASK mode2/material359 at00:15:46/00:15:54Z, then BLEND
  mode1/material4772 at00:16:11Z, with tint alpha1. Mode1/reason10 is the rigged
  gradient-alpha keep-blend case, not the earlier invisible face's reason6.
- A working face also logged: row2936/face2, bake7, MASK mode2, alpha1, skin bit1.
- Both normal and alpha cluster draws resolve the same selected skin map and
  welded output through clusterSkinnedVertex. The tier2 producer/task/mesh record
  indexing agrees in the source. No incorrect index was proven.
- Original diagnostic signature mixing was inside clean_static. A successfully
  classified rigged mesh does not take that branch; an unrigged rigid attachment
  does. Thus the existence of the signature mix is not proof of this rigged gap.

## Diagnostic containment in this change

RenderVKSkinFaceDiag is now a read-only observer of the committed instance row,
including when ordinary ingest deduplicates it. Its selection/debug state is no
longer part of the registration signature. It cannot upsert a row, allocate a
palette, change an LOD group, or request a cluster rebuild. Skin classification
has been factored without changing its rule. Numeric lines additionally expose
stored tint alpha, row flags, skin ID, LOD group and the object's current source
LOD, so a stale binding can be compared with the live TE facts.

The production observer fixture uses read-only API stubs; any writer added to
that function fails its contract/compile. Repeated diagnostic toggles preserve
the row's model, skin, group and material bytes. Actual llvovolume.cpp syntax and
the source-bound alpha fixture pass. No viewer/GPU run or visual repair claim.

## Next decisive owner checks

1. Restore row3310's original MASK alpha mode without changing selection/geometry.
   Did alignment recover? Was the MASK-to-BLEND edit intentional, and did the gap
   appear before or after that edit?
2. Re-wear result: did a new object/instance binding clear it? Re-wear rebuilds row
   bindings but may reuse the mesh DAG/cache. A relog recreates runtime skin/rest
   state too, but neither action guarantees repair of a deterministic asset/cut
   or alpha-route problem. Do not promise either clears it.
3. If still present, obtain the new numeric binding line for the bad face and a
   neighbour, and compare CLOD masked versus blended rendering at identical pose.

Pending those facts, keep select-gap OPEN. Do not delete .clod caches or force
unrelated geometry rebuilds as a substitute for identifying the failing input.
