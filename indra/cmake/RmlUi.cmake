# RmlUi 6.3: official MIT release, immutable archive digest. No platform backend.
include_guard(GLOBAL)
include(FetchContent)
if(NOT TARGET ll::freetype)
  include(FreeType)
endif()

# RmlUi deliberately accepts a parent-supplied FreeType target. Keep it on the
# viewer's prebuilt library (and its transitive PNG/Brotli/zlib dependencies).
if(NOT TARGET Freetype::Freetype)
  add_library(Freetype::Freetype ALIAS ll::freetype)
endif()

function(sl_add_rmlui)
  # Function scope prevents upstream's generic BUILD_SHARED_LIBS option from
  # changing any viewer target. CMP0077 honors these ordinary variables.
  set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
  endif()
  set(BUILD_SHARED_LIBS OFF)
  set(RMLUI_SAMPLES OFF)
  set(RMLUI_TESTS OFF)
  set(RMLUI_SHELL OFF)
  set(RMLUI_LUA_BINDINGS OFF)
  set(RMLUI_LOTTIE_PLUGIN OFF)
  set(RMLUI_SVG_PLUGIN OFF)
  set(RMLUI_FONT_ENGINE freetype)
  set(RMLUI_PRECOMPILED_HEADERS OFF)
  set(RMLUI_THIRDPARTY_CONTAINERS OFF)
  set(RMLUI_MATRIX_ROW_MAJOR OFF)
  set(RMLUI_COMPILER_OPTIONS ON)
  FetchContent_Declare(rmlui
    URL https://codeload.github.com/mikke89/RmlUi/tar.gz/refs/tags/6.3
    URL_HASH SHA256=d977298bb6147610e5984d5db85ddf284020d655a8713913f6982074f1dbdede
    PATCH_COMMAND "${CMAKE_COMMAND}" "-DRMLUI_SOURCE_DIR=<SOURCE_DIR>"
      -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PatchRmlUiFontAllocator.cmake")
  FetchContent_MakeAvailable(rmlui)
endfunction()
sl_add_rmlui()
