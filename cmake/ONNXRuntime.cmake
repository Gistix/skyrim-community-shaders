# cmake/ONNXRuntime.cmake - ONNX Runtime DirectML / Tensor Core integration.

set(ONNXRUNTIME_DIR "${CMAKE_SOURCE_DIR}/extern/onnxruntime")

add_library(onnxruntime SHARED IMPORTED GLOBAL)
set_target_properties(onnxruntime PROPERTIES
    IMPORTED_LOCATION "${ONNXRUNTIME_DIR}/bin/onnxruntime.dll"
    IMPORTED_IMPLIB "${ONNXRUNTIME_DIR}/lib/onnxruntime.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_DIR}/include"
)

target_link_libraries(
    ${PROJECT_NAME}
    PRIVATE
        onnxruntime
        delayimp
)

if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE "/DELAYLOAD:onnxruntime.dll")
endif()

set(ONNXRUNTIME_BIN_NAMES
    onnxruntime.dll
    onnxruntime_providers_shared.dll
)

set(_ONNXRUNTIME_FILES
    "${ONNXRUNTIME_DIR}/bin/onnxruntime.dll"
    "${ONNXRUNTIME_DIR}/bin/onnxruntime_providers_shared.dll"
)

install(
    FILES ${_ONNXRUNTIME_FILES}
    DESTINATION SKSE/Plugins/CommunityShaders/bin
    COMPONENT ONNXRuntime
)

set(_ONNXRUNTIME_AIO_RUNTIME_DIR "${CMAKE_CURRENT_BINARY_DIR}/aio/SKSE/Plugins/CommunityShaders/bin")
add_custom_command(
    TARGET ${PROJECT_NAME}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_ONNXRUNTIME_AIO_RUNTIME_DIR}"
    COMMAND
        ${CMAKE_COMMAND} -E copy_if_different
        "${ONNXRUNTIME_DIR}/bin/onnxruntime.dll"
        "${_ONNXRUNTIME_AIO_RUNTIME_DIR}/"
    COMMAND
        ${CMAKE_COMMAND} -E copy_if_different
        "${ONNXRUNTIME_DIR}/bin/onnxruntime_providers_shared.dll"
        "${_ONNXRUNTIME_AIO_RUNTIME_DIR}/"
    VERBATIM
)
