# vqbench (#1039) -- standalone timing bench for the visibility queue: the viewer's GI split and pass-15 shadow-core
# kernels (the viewer's sources, the viewer's defines: vqb_spv.py imports regen_world_spv.py's table) on a
# reproducible synthetic scene, the viewer's default chain vs the queue's A/B configurations. Not part of the viewer
# build.
#
#   cmake -S indra/vqbench -B build-vqbench -G Ninja && ninja -C build-vqbench
#   indra/vqbench/vqb_run.sh build-vqbench/vqbench [options]      (takes the GPU lock; see README.md)
cmake_minimum_required(VERSION 3.20)
project(vqbench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

find_package(Vulkan REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(GLSLC glslc REQUIRED)

set(VKCLEAN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../vkclean)
set(SPV_DIR ${CMAKE_CURRENT_BINARY_DIR}/spv)
file(GLOB VKCLEAN_SHADERS ${VKCLEAN_DIR}/shaders/*.glsl ${VKCLEAN_DIR}/shaders/*.comp)

# every kernel + its names twin + the bench's own shaders; re-run when any viewer shader or the table changes
add_custom_command(
    OUTPUT ${SPV_DIR}/kernels.txt
    COMMAND ${CMAKE_COMMAND} -E env GLSLC=${GLSLC}
            ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/vqb_spv.py --out ${SPV_DIR} --cache ${CMAKE_CURRENT_BINARY_DIR}/spvcache
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/vqb_spv.py ${VKCLEAN_DIR}/regen_world_spv.py ${VKCLEAN_DIR}/spvbuild.py
            ${VKCLEAN_SHADERS} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/vqb_gbuf.comp
    COMMENT "vqbench: compiling the viewer's lighting kernels (vqb_spv.py)")
add_custom_target(vqbench_spv ALL DEPENDS ${SPV_DIR}/kernels.txt)

add_executable(vqbench vqb_main.cpp vqb_vk.cpp vqb_reflect.cpp vqb_scene.cpp)
target_include_directories(vqbench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
target_link_libraries(vqbench PRIVATE Vulkan::Vulkan)
target_compile_definitions(vqbench PRIVATE VQB_SPV_DIR="${SPV_DIR}")
add_dependencies(vqbench vqbench_spv)
