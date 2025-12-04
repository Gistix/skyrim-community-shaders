cmake_minimum_required(VERSION 3.21)

# NVIDIA RTXGI-DDGI SDK Integration
# Using Unmanaged Resource Mode (RTXGI_DDGI_RESOURCE_MANAGEMENT=0)
# This allows integration with existing descriptor heap and resource management

set(RTXGI_ROOT ${CMAKE_SOURCE_DIR}/extern/RTXGI-DDGI/rtxgi-sdk)

# Create RTXGI library with SDK source files
add_library(RTXGI STATIC
    ${RTXGI_ROOT}/src/ddgi/DDGIVolume.cpp
    ${RTXGI_ROOT}/src/ddgi/gfx/DDGIVolume_D3D12.cpp
    ${RTXGI_ROOT}/src/Math.cpp
)

# Require C++17 for std::clamp
target_compile_features(RTXGI PUBLIC cxx_std_17)

target_include_directories(RTXGI PUBLIC
    ${RTXGI_ROOT}/include
)

# Use Unmanaged Resource Mode - application manages all resources
target_compile_definitions(RTXGI PUBLIC
    RTXGI_DDGI_RESOURCE_MANAGEMENT=0
    RTXGI_COORDINATE_SYSTEM=0  # Left-handed Y-up (RTXGI_COORDINATE_SYSTEM_LEFT)
)

# Disable warnings for third-party code
if(MSVC)
    target_compile_options(RTXGI PRIVATE
        /W0  # Disable all warnings for SDK code
    )
endif()

# Link D3D12
target_link_libraries(RTXGI PUBLIC
    d3d12.lib
    dxgi.lib
)
