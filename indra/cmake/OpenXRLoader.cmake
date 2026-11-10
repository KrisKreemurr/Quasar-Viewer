# -*- cmake -*-
#
# The Khronos OpenXR loader for Windows (#1401, VR). The viewer loads it at run time
# (vkgfx/llxr.cpp: LoadLibraryExW "openxr_loader.dll" from the executable folder), so it
# has to ship beside slipstream.exe: without it VR reports "the OpenXR loader
# (openxr_loader.dll) was not found" on any machine that has no loader of its own on PATH
# (almost all of them; SteamVR / Oculus / WMR install a *runtime*, not the loader).
#
# Khronos publishes the official x64 build only inside the OpenXR.Loader NuGet package of
# each OpenXR-SDK release (a zip; Apache-2.0 OR MIT). It is fetched and verified (SHA256)
# into the 3p download cache (SL_3P_CACHE_DIR) under its own name, so offline rebuilds work
# from there, and the DLL (imports only KERNEL32 / ADVAPI32; static CRT) is staged into
# SHARED_LIB_STAGING_DIR for viewer_manifest.py. To bump: change OPENXR_LOADER_VERSION and
# OPENXR_LOADER_SHA256. Linux does not ship the loader: it comes from the distribution's
# openxr package (libopenxr_loader.so.1).
include_guard()

include(Prebuilt)   # SL_3P_CACHE_DIR

if (NOT WINDOWS OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|x64)$")
  return()
endif ()

set(OPENXR_LOADER_VERSION 1.1.65)
set(OPENXR_LOADER_SHA256 "05638c526f07ba3a5f1084bbc3308d654d5167f844ffa1e35515325cb72e0c34")
set(OPENXR_LOADER_URL
    "https://github.com/KhronosGroup/OpenXR-SDK/releases/download/release-${OPENXR_LOADER_VERSION}/OpenXR.Loader.${OPENXR_LOADER_VERSION}.nupkg"
    CACHE STRING "OpenXR.Loader NuGet package (a mirror must serve the same bytes)")

set(_xrl_dir "${CMAKE_BINARY_DIR}/3p-src/openxr-loader-${OPENXR_LOADER_VERSION}")
set(_xrl_dll "${_xrl_dir}/native/x64/release/bin/openxr_loader.dll")
set(_xrl_archive "${SL_3P_CACHE_DIR}/OpenXR.Loader.${OPENXR_LOADER_VERSION}.nupkg")

if (NOT EXISTS "${_xrl_dll}")
  set(_xrl_ok FALSE)
  if (EXISTS "${_xrl_archive}")
    file(SHA256 "${_xrl_archive}" _xrl_got)
    if (_xrl_got STREQUAL "${OPENXR_LOADER_SHA256}")
      set(_xrl_ok TRUE)
    else ()
      message(WARNING "OpenXR loader: ${_xrl_archive} does not match its SHA256; downloading it again")
      file(REMOVE "${_xrl_archive}")
    endif ()
  endif ()
  if (NOT _xrl_ok)
    file(MAKE_DIRECTORY "${SL_3P_CACHE_DIR}")
    string(RANDOM LENGTH 8 _rnd)
    set(_part "${_xrl_archive}.part-${_rnd}")
    message(STATUS "OpenXR loader: downloading ${OPENXR_LOADER_VERSION} <- ${OPENXR_LOADER_URL}")
    file(DOWNLOAD "${OPENXR_LOADER_URL}" "${_part}"
         STATUS _status
         TLS_VERIFY ON
         INACTIVITY_TIMEOUT 120
         EXPECTED_HASH SHA256=${OPENXR_LOADER_SHA256})
    list(GET _status 0 _code)
    if (NOT _code EQUAL 0)
      list(GET _status 1 _msg)
      file(REMOVE "${_part}")
      message(FATAL_ERROR "OpenXR loader: failed to download ${OPENXR_LOADER_URL}: ${_msg}")
    endif ()
    file(RENAME "${_part}" "${_xrl_archive}")
  endif ()
  message(STATUS "OpenXR loader: unpacking ${OPENXR_LOADER_VERSION}")
  file(REMOVE_RECURSE "${_xrl_dir}")
  file(ARCHIVE_EXTRACT INPUT "${_xrl_archive}" DESTINATION "${_xrl_dir}"
       PATTERNS "native/x64/release/bin/openxr_loader.dll")
  if (NOT EXISTS "${_xrl_dll}")
    message(FATAL_ERROR "OpenXR loader: ${_xrl_archive} did not contain native/x64/release/bin/openxr_loader.dll")
  endif ()
endif ()

# Copy beside the other staged shared libraries (the viewer target depends on it, newview).
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/openxr_loader.staged"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${SHARED_LIB_STAGING_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_xrl_dll}" "${SHARED_LIB_STAGING_DIR}/openxr_loader.dll"
  COMMAND ${CMAKE_COMMAND} -E touch "${CMAKE_BINARY_DIR}/openxr_loader.staged"
  DEPENDS "${_xrl_dll}"
  VERBATIM)
add_custom_target(openxr_loader_stage ALL DEPENDS "${CMAKE_BINARY_DIR}/openxr_loader.staged")
