# -*- cmake -*-
#
# Quasar: NVIDIA's Streamline SDK, for Reflex and DLSS Frame Generation on the
# Vulkan presentation path (indra/llwindow/qlvulkanpresenter.cpp). Windows only.
#
# The SDK is fetched from NVIDIA's GitHub release when the build is configured,
# rather than kept in the repository: its headers are MIT licensed, but the
# runtime DLLs are NVIDIA's, under the RTX SDKs and Reflex licences shipped
# beside them. The viewer loads sl.interposer.dll at run time, after checking
# NVIDIA's signature on it, and only when QuasarStreamline is on; the build
# itself links nothing of it. A configure that cannot fetch it goes on without:
# QL_HAS_STREAMLINE is then left undefined and the presenter loads Vulkan alone.
include_guard()
add_library(ql::streamline INTERFACE IMPORTED)

if(WINDOWS)
  option(QUASAR_STREAMLINE "Fetch NVIDIA Streamline (Reflex, DLSS Frame Generation)" ON)
endif()

if(WINDOWS AND QUASAR_STREAMLINE)
  set(ql_sl_version "2.14.16")
  set(ql_sl_sha256 "32041e51db0cab709a271b72a352281b690c2abbd669a827809401a5dd23956b")
  set(ql_sl_dir "${CMAKE_BINARY_DIR}/_deps/streamline-${ql_sl_version}")
  set(ql_sl_zip "${CMAKE_BINARY_DIR}/_deps/streamline-sdk-v${ql_sl_version}.zip")

  if(NOT EXISTS "${ql_sl_dir}/include/sl.h")
    message(STATUS "Fetching NVIDIA Streamline ${ql_sl_version} (about 270 MB, once)")
    file(
      DOWNLOAD
        "https://github.com/NVIDIA-RTX/Streamline/releases/download/v${ql_sl_version}/streamline-sdk-v${ql_sl_version}.zip"
        "${ql_sl_zip}"
      EXPECTED_HASH SHA256=${ql_sl_sha256}
      STATUS ql_sl_status
    )
    list(GET ql_sl_status 0 ql_sl_code)
    if(ql_sl_code EQUAL 0)
      file(ARCHIVE_EXTRACT INPUT "${ql_sl_zip}" DESTINATION "${ql_sl_dir}")
      file(REMOVE "${ql_sl_zip}")
    else()
      list(GET ql_sl_status 1 ql_sl_error)
      message(WARNING "NVIDIA Streamline not fetched (${ql_sl_error}): building without Reflex and DLSS-G")
      file(REMOVE "${ql_sl_zip}")
    endif()
  endif()

  if(EXISTS "${ql_sl_dir}/include/sl.h")
    target_include_directories(ql::streamline SYSTEM INTERFACE "${ql_sl_dir}/include")
    target_compile_definitions(ql::streamline INTERFACE QL_HAS_STREAMLINE=1)

    # The signed production binaries the viewer loads, staged beside it by
    # ViewerInstall.cmake with the licences that cover them.
    set(ql_sl_bin "${ql_sl_dir}/bin/x64")
    set(
      QL_STREAMLINE_RUNTIME_FILES
      "${ql_sl_bin}/sl.interposer.dll"
      "${ql_sl_bin}/sl.common.dll"
      "${ql_sl_bin}/sl.pcl.dll"
      "${ql_sl_bin}/sl.reflex.dll"
      "${ql_sl_bin}/sl.dlss_g.dll"
      "${ql_sl_bin}/nvngx_dlssg.dll"
      "${ql_sl_bin}/NvLowLatencyVk.dll"
      "${ql_sl_bin}/nvngx_dlss.license.txt"
      "${ql_sl_bin}/reflex.license.txt"
    )
    set(QL_STREAMLINE_LICENSE "${ql_sl_dir}/license.txt")
  endif()
endif()
