cmake_minimum_required(VERSION 3.15)
project(PixelVRStream LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Opción para habilitar OpenVR (ON por defecto)
option(USE_OPENVR "Enable OpenVR integration" ON)

# Ruta por defecto al SDK de OpenVR (relativa al root del proyecto)
set(OPENVR_ROOT "${CMAKE_SOURCE_DIR}/lib/openvr" CACHE PATH "Path to OpenVR SDK (contains headers/ and lib/win64/)")

# Fuentes del target
add_library(PixelVRStream SHARED
    src/PixelVRStream.cpp
    src/OpenVRCapture.cpp
    src/MFEncoder.cpp
    src/NetworkRTP.cpp
)

# Export macro para las funciones
target_compile_definitions(PixelVRStream
    PRIVATE PIXELVRSTREAM_EXPORTS
)

# Includes del proyecto
target_include_directories(PixelVRStream
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# OpenVR: añade includes y lib si está habilitado
if(USE_OPENVR)
    message(STATUS "OpenVR integration requested. OPENVR_ROOT=${OPENVR_ROOT}")

    if(EXISTS "${OPENVR_ROOT}/headers")
        target_include_directories(PixelVRStream PRIVATE "${OPENVR_ROOT}/headers")
    else()
        message(WARNING "OpenVR headers not found at ${OPENVR_ROOT}/headers. Please set -DOPENVR_ROOT to the correct location.")
    endif()

    # Intentar encontrar la import library en la ruta esperada
    set(OPENVR_LIB_PATH "${OPENVR_ROOT}/lib/win64/openvr_api.lib")
    if(EXISTS "${OPENVR_LIB_PATH}")
        target_link_directories(PixelVRStream PRIVATE "${OPENVR_ROOT}/lib/win64")
        target_link_libraries(PixelVRStream PRIVATE openvr_api)
    else()
        message(WARNING "openvr_api.lib not found at ${OPENVR_LIB_PATH}. If you only have openvr_api.dll, copy it next to the executable at runtime or provide the .lib.")
    endif()

    target_compile_definitions(PixelVRStream PRIVATE USE_OPENVR=1)
else()
    message(STATUS "OpenVR integration disabled (USE_OPENVR=OFF).")
endif()

# Linker libraries specific to Windows (Media Foundation, D3D11, winsock)
if(WIN32)
    target_link_libraries(PixelVRStream PRIVATE
        mfplat
        mf
        mfreadwrite
        mfuuid
        d3d11
        dxgi
        Ws2_32
    )
endif()

# Output directories (por configuración)
set_target_properties(PixelVRStream PROPERTIES
    OUTPUT_NAME "PixelVRStream"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
)

# Mensajes útiles
message(STATUS "CMake generator: ${CMAKE_GENERATOR}")
message(STATUS "Build type(s): ${CMAKE_CONFIGURATION_TYPES}")