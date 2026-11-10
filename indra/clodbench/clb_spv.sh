#!/bin/sh
# clodbench: compile the cluster RT kernel builds to compare, with the viewer's flags (regen_world_spv.py:
# glslc --target-env=vulkan1.3 -O -DCLEAN_STAGE_COMP).
#   clb_spv.sh <vkclean dir> <out dir>
#   uber.spv   every mode in one pipeline (the viewer before 2026-11-05)
#   lite.spv   every mode except 0 and 13 (-DCL_RT_LITE)
#   sel.spv    mode 0 alone (-DCL_RT_ONLY_SELECT)
#   admit.spv  mode 13 alone (-DCL_RT_ONLY_ADMIT)
set -e
src="$1/shaders/clean_cluster_rt.comp"
out="$2"
mkdir -p "$out"
g="glslc --target-env=vulkan1.3 -O -fshader-stage=comp -DCLEAN_STAGE_COMP"
$g "$src" -o "$out/uber.spv"
$g -DCL_RT_LITE "$src" -o "$out/lite.spv"
$g -DCL_RT_ONLY_SELECT "$src" -o "$out/sel.spv"
$g -DCL_RT_ONLY_ADMIT "$src" -o "$out/admit.spv"
echo "clb_spv: $out/{uber,lite,sel,admit}.spv"
