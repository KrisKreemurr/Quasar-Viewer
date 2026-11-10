# shadowbench -- standalone harness for the Vulkan renderer's raster (shadow-mapped) sun
# cascades. Runs the viewer's own cascade fit + cache schedule (vkgfx/llvkshadowcascades.cpp),
# the viewer's caster shaders (depth-only world VS, shadow mask / translucency FS) and the
# viewer's receiver-side shadow evaluation (clean_draw_common.glsl sunShadowT) on a synthetic
# scene, timing each pass and diffing the receiver shadow image between policies.
#
#   cmake -S indra/shadowbench -B build-shadowbench -G Ninja && ninja -C build-shadowbench
#   build-shadowbench/shadowbench_sim                     (CPU only: re-render rates)
#   build-shadowbench/shadowbench --help                  (GPU: timings + image diffs)
cmake_minimum_required(VERSION 3.20)
project(shadowbench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()
# The viewer's Linux release ISA: the fit's float arithmetic matches it bit for bit.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=x86-64-v2")

find_package(Vulkan REQUIRED)

set(VKTEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../vktest)
set(VKGFX_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../newview/vkgfx)
set(VKCLEAN_SHADERS ${CMAKE_CURRENT_SOURCE_DIR}/../vkclean/shaders)

add_executable(shadowbench_sim sb_sim.cpp ${VKGFX_DIR}/llvkshadowcascades.cpp)
target_include_directories(shadowbench_sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${VKGFX_DIR})

add_executable(shadowbench
    sb_main.cpp
    sb_gpu.cpp
    ${VKGFX_DIR}/llvkshadowcascades.cpp
    ${VKTEST_DIR}/vk_device.cpp
)
# (vktest first: vkgfx has its own vk_common.h / vk_device.h, the viewer's)
target_include_directories(shadowbench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${VKTEST_DIR} ${VKGFX_DIR} ${Vulkan_INCLUDE_DIRS})
target_link_libraries(shadowbench PRIVATE Vulkan::Vulkan)

# ---- shaders: the viewer's (as regen_world_spv.py compiles them) + the harness's own ----
find_program(GLSLC glslc REQUIRED)
set(SPV_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SPV_DIR})
set(SPV_OUTPUTS)
function(sb_shader SRC STAGE OUT)
    set(OUTF ${SPV_DIR}/${OUT}.spv)
    add_custom_command(
        OUTPUT ${OUTF}
        COMMAND ${GLSLC} --target-env=vulkan1.3 -O -fshader-stage=${STAGE} -I ${VKCLEAN_SHADERS} ${ARGN} ${SRC} -o ${OUTF}
        DEPENDS ${SRC}
        IMPLICIT_DEPENDS CXX ${SRC}
        COMMENT "glslc ${OUT}")
    set(SPV_OUTPUTS ${SPV_OUTPUTS} ${OUTF} PARENT_SCOPE)
endfunction()
sb_shader(${VKCLEAN_SHADERS}/clean_draw.vert vert kWorldVertDepthSpv -DCLEAN_DEPTH_ONLY)
sb_shader(${VKCLEAN_SHADERS}/clean_draw.vert vert kWorldVertDepthMaskSpv -DCLEAN_DEPTH_ONLY -DCLEAN_DEPTH_MASK)
sb_shader(${VKCLEAN_SHADERS}/clean_shadow_mask.frag frag kWorldShadowMaskFragSpv)
sb_shader(${VKCLEAN_SHADERS}/clean_shadow_trans.frag frag kWorldShadowTransDepthFragSpv)
sb_shader(${VKCLEAN_SHADERS}/clean_shadow_trans.frag frag kWorldShadowTransColorFragSpv -DTRANS_COLOR)
sb_shader(${CMAKE_CURRENT_SOURCE_DIR}/shaders/sb_gbuf.vert vert sb_gbuf.vert)
sb_shader(${CMAKE_CURRENT_SOURCE_DIR}/shaders/sb_gbuf.frag frag sb_gbuf.frag)
sb_shader(${CMAKE_CURRENT_SOURCE_DIR}/shaders/sb_cull.comp comp sb_cull.comp)
sb_shader(${CMAKE_CURRENT_SOURCE_DIR}/shaders/sb_receiver.comp comp sb_receiver.comp)
add_custom_target(shadowbench_shaders ALL DEPENDS ${SPV_OUTPUTS})
add_dependencies(shadowbench shadowbench_shaders)
target_compile_definitions(shadowbench PRIVATE SB_SHADER_DIR="${SPV_DIR}")
