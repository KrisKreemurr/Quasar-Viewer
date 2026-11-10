# clodbench (CLOD perf, 2026-11-05) -- standalone timing bench for the cluster LOD RT build kernels
# (indra/vkclean/shaders/clean_cluster_rt.comp) on a synthetic avatar-heavy steady state. Not part of the
# viewer build. The SPIR-V builds to compare are compiled by clb_spv.sh and passed with --variant.
#
#   cmake -S indra/clodbench -B <dir> -G Ninja && ninja -C <dir>
#   indra/clodbench/clb_spv.sh <vkclean dir> <out dir>       (uber.spv, lite.spv, ...)
#   indra/clodbench/clb_run.sh <dir>/clodbench --variant uber=<out>/uber.spv --variant lite=<out>/lite.spv
cmake_minimum_required(VERSION 3.20)
project(clodbench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

find_package(Vulkan REQUIRED)
add_executable(clodbench clb_main.cpp vqb_vk.cpp vqb_reflect.cpp)
target_include_directories(clodbench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${Vulkan_INCLUDE_DIRS})
target_link_libraries(clodbench PRIVATE Vulkan::Vulkan)
