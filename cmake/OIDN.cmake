# cmake/OIDN.cmake - Intel Open Image Denoise (OIDN) integration.
#
# OIDN's GPU device consumes external Vulkan buffers. The plugin runs its
# prepare/composite passes on D3D11 and bridges the resources to OIDN through
# DXVK's Vulkan device (see src/OIDNDenoiser.cpp).

set(OIDN_APPS OFF CACHE BOOL "" FORCE)
set(OIDN_FILTER_RTLIGHTMAP OFF CACHE BOOL "" FORCE)
set(OIDN_FILTER_RT ON CACHE BOOL "" FORCE)
set(OIDN_DEVICE_CPU OFF CACHE BOOL "" FORCE)
set(OIDN_DEVICE_CUDA ON CACHE BOOL "" FORCE)
set(OIDN_DEVICE_SYCL OFF CACHE BOOL "" FORCE)
set(OIDN_DEVICE_HIP OFF CACHE BOOL "" FORCE)
set(OIDN_WARN_AS_ERRORS OFF CACHE BOOL "" FORCE)

# Normalize the toolchain path: the CUDA backend is built by an ExternalProject that
# writes the toolchain into an initial-cache file, where Windows backslashes are
# parsed as escapes (e.g. \R in H:\Repos\...).
if(CMAKE_TOOLCHAIN_FILE)
    file(TO_CMAKE_PATH "${CMAKE_TOOLCHAIN_FILE}" _normalized_toolchain)
    set(CMAKE_TOOLCHAIN_FILE "${_normalized_toolchain}" CACHE FILEPATH "" FORCE)
endif()

# Save CMAKE_INTERPROCEDURAL_OPTIMIZATION and disable it for OIDN:
# WINDOWS_EXPORT_ALL_SYMBOLS (__create_def) cannot parse MSVC /GL (LTO/IPO) object files.
set(_SAVED_CMAKE_IPO "${CMAKE_INTERPROCEDURAL_OPTIMIZATION}")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)

# Disable /WX for the OIDN subproject.
set(_SAVED_CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
string(REGEX REPLACE "/WX([ ]|$)" "/WX- " CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /WX- /wd4201 /wd4458")

add_subdirectory(extern/OIDN EXCLUDE_FROM_ALL)

set(CMAKE_CXX_FLAGS "${_SAVED_CMAKE_CXX_FLAGS}")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION "${_SAVED_CMAKE_IPO}")

foreach(_oidn_target OpenImageDenoise_common OpenImageDenoise_weights OpenImageDenoise_core OpenImageDenoise)
    if(TARGET ${_oidn_target})
        target_compile_options(${_oidn_target} PRIVATE /WX- /wd4201 /wd4458)
    endif()
endforeach()

target_link_libraries(
    ${PROJECT_NAME}
    PRIVATE
        OpenImageDenoise
        delayimp
)

if(MSVC)
    # Delay-load so a missing OIDN runtime cannot stop CommunityShaders.dll loading.
    target_link_options(${PROJECT_NAME} PRIVATE "/DELAYLOAD:OpenImageDenoise.dll")
endif()

if(TARGET OpenImageDenoise_device_cuda)
    add_dependencies(${PROJECT_NAME} OpenImageDenoise_device_cuda)
endif()

target_include_directories(
    ${PROJECT_NAME}
    PRIVATE
        "${CMAKE_SOURCE_DIR}/extern/OIDN/include"
)

# Runtime DLLs shipped next to the plugin (CommunityShaders/bin).
# OpenImageDenoise.dll and OpenImageDenoise_core.dll are the linked libraries; the
# CUDA backend is a runtime-loaded module placed next to OpenImageDenoise.dll.
set(OIDN_RUNTIME_BIN_NAMES OpenImageDenoise.dll)
if(TARGET OpenImageDenoise_core)
    list(APPEND OIDN_RUNTIME_BIN_NAMES OpenImageDenoise_core.dll)
endif()
if(TARGET OpenImageDenoise_device_cuda)
    list(APPEND OIDN_RUNTIME_BIN_NAMES OpenImageDenoise_device_cuda.dll)
endif()
# NOTE: these names must also be registered in runtime_paths.txt (top-level
# CMakeLists) or cmake/CleanupStaleEntries.cmake deletes the post-build-staged
# DLLs from the AIO folder as stale entries.
set(_OIDN_RUNTIME_FILES "$<TARGET_FILE:OpenImageDenoise>")
if(TARGET OpenImageDenoise_core)
    list(APPEND _OIDN_RUNTIME_FILES "$<TARGET_FILE:OpenImageDenoise_core>")
endif()
if(TARGET OpenImageDenoise_device_cuda)
    list(APPEND _OIDN_RUNTIME_FILES "$<TARGET_FILE_DIR:OpenImageDenoise>/OpenImageDenoise_device_cuda.dll")
endif()

install(
    FILES ${_OIDN_RUNTIME_FILES}
    DESTINATION SKSE/Plugins/CommunityShaders/bin
    COMPONENT OIDN
)

# Mirror the install into the glob-based AIO staging so auto-deployment picks it up.
set(_OIDN_AIO_RUNTIME_DIR "${CMAKE_CURRENT_BINARY_DIR}/aio/SKSE/Plugins/CommunityShaders/bin")
add_custom_command(
    TARGET ${PROJECT_NAME}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_OIDN_AIO_RUNTIME_DIR}"
    COMMAND
        ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:OpenImageDenoise>"
        "${_OIDN_AIO_RUNTIME_DIR}/"
    VERBATIM
)
if(TARGET OpenImageDenoise_core)
    add_custom_command(
        TARGET ${PROJECT_NAME}
        POST_BUILD
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:OpenImageDenoise_core>"
            "${_OIDN_AIO_RUNTIME_DIR}/"
        VERBATIM
    )
endif()
if(TARGET OpenImageDenoise_device_cuda)
    add_custom_command(
        TARGET ${PROJECT_NAME}
        POST_BUILD
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE_DIR:OpenImageDenoise>/OpenImageDenoise_device_cuda.dll"
            "${_OIDN_AIO_RUNTIME_DIR}/"
        VERBATIM
    )
endif()
