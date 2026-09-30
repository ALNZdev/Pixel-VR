cmake_minimum_required(VERSION 3.10)

# This is your project name
set(TARGET_NAME pixelvr)

# This is the name that the binary will be called (driver_<my_driver_name>)
set(DRIVER_NAME "driver_${TARGET_NAME}")

project(${TARGET_NAME})

set(OPENVR_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib/openvr)
set(PSMOVE_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib/PSMoveClient_CAPI)

# If not set, determine the platform architecture
if (NOT PLATFORM)
    if (CMAKE_SIZEOF_VOID_P MATCHES 8)
        set(PLATFORM 64)
    else ()
        set(PLATFORM 32)
    endif ()
endif ()
message(STATUS "Compilation set for ${PLATFORM}bits architectures.")

# OpenVR compatibility checking
if (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    add_definitions(-DLINUX -DPOSIX)
    set(ARCH_TARGET linux64)

    if (${PLATFORM} MATCHES 32)
        message(WARNING "OpenVR x86 binaries not provided on GNU/Linux.")
    endif ()
elseif (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    set(CMAKE_MACOSX_RPATH 0)
    add_definitions(-DOSX -DPOSIX)
    set(ARCH_TARGET osx32)

elseif (${CMAKE_SYSTEM_NAME} MATCHES "Windows")
    add_definitions(-D_WIN32)
    set(ARCH_TARGET win${PLATFORM})

    # Binaries path for thirdparties are not generics so we try to guess their suffixes.
    set(WINDOWS_PATH_SUFFIXES win${PLATFORM} Win${PLATFORM} x${PLATFORM})
endif ()

# ✅ OPENVR
find_library(OPENVR_LIBRARIES
        NAMES
        openvr_api
        PATHS
        ${OPENVR_LIB_DIR}/bin
        ${OPENVR_LIB_DIR}/lib
        PATH_SUFFIXES
        osx${PLATFORM}
        linux${PLATFORM}
        win${PLATFORM}
        NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH
        )

# ✅ PSMOVE (SEPARADO)
find_library(PSMOVE_LIBRARIES
        NAMES
        PSMoveClient_CAPI
        PATHS
        ${PSMOVE_LIB_DIR}/lib
        NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH
        )

set(OPENVR_INCLUDE_DIR ${OPENVR_LIB_DIR}/headers)
set(PSMOVE_INCLUDE_DIR ${PSMOVE_LIB_DIR}/include)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY $<1:${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}/bin/${ARCH_TARGET}>)

# ============================================================================
# ARCHIVOS DEL DRIVER - ORIGINALES + NUEVOS STREAMING
# ============================================================================

add_library(${DRIVER_NAME} SHARED
        # ─── Archivos Originales ───
        src/main.cpp
        src/main.h
        src/hmd.cpp
        src/hmd.h
        src/controllers.cpp
        src/controllers.h
        src/driver_factory.cpp
        src/pipe_handler.cpp
        src/pipe_handler.h
        src/driverlog.cpp
        src/driverlog.h
        src/vrmath.h
        src/psm_tracking_manager.cpp
        src/psm_tracking_manager.h
        
        # ─── NUEVOS: Sistema Universal de Encoders de Video GPU ───
        src/video_encoder.h
        src/encoders/video_encoder_factory.cpp
        
        # Encoder NVIDIA NVENC
        src/encoders/video_encoder_nvidia.h
        src/encoders/video_encoder_nvidia.cpp
        
        # Encoder AMD VCN
        src/encoders/video_encoder_amd.h
        src/encoders/video_encoder_amd.cpp
        
        # Encoder Intel QuickSync
        src/encoders/video_encoder_intel.h
        src/encoders/video_encoder_intel.cpp
        
        # ─── Helpers D3D11 ───
        src/d3d11_helper.h
        src/d3d11_helper.cpp

        # ─── Modo Android (USB): IVRDriverDirectModeComponent -> SBS (GPU) -> AMF -> TCP ───
        src/direct_mode.h
        src/direct_mode.cpp
        src/sbs_composer.h
        src/sbs_composer.cpp
        
        # ─── NUEVOS: Servidor de Streaming TCP ───
        src/streaming_server.h
        src/streaming_server.cpp
        
        # ─── NUEVOS: Gestor de Configuración ───
        src/config_manager.h
        src/config_manager.cpp
        src/display_mode.h
        src/display_mode.cpp
        src/stream_protocol.h
        # video_encoder_factory.cpp todavia referencia el encoder Media Foundation (fallback)
        src/encoders/video_encoder_mf.h
        src/encoders/video_encoder_mf.cpp
        src/video_stream_pipeline.h
        src/video_stream_pipeline.cpp
        )

# ============================================================================
# LIBRERÍAS A LINKEAR
# ============================================================================

target_link_libraries(${DRIVER_NAME} PRIVATE ${OPENVR_LIBRARIES} ${PSMOVE_LIBRARIES})

if (WIN32)
    target_compile_definitions(${DRIVER_NAME} PRIVATE NOMINMAX)
endif ()

# Windows: Librerías específicas para D3D11 y Networking
if (WIN32)
    target_link_libraries(${DRIVER_NAME} PRIVATE
        d3d11.lib          # Direct3D 11
        dxgi.lib           # DXGI (adaptadores gráficos)
        dxguid.lib         # DXGI GUIDs
        d3dcompiler.lib    # Compilacion runtime del shader SBS
        ws2_32.lib         # Winsock 2 (TCP/IP)
        mfplat.lib         # Solo por video_encoder_mf (fallback de la factory)
        mf.lib
        mfuuid.lib
        strmiids.lib
        ole32.lib
        )
endif ()

# ============================================================================
# DIRECTORIOS DE INCLUDE
# ============================================================================

target_include_directories(${DRIVER_NAME} PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OPENVR_INCLUDE_DIR} 
    ${PSMOVE_INCLUDE_DIR}
    )

# ============================================================================
# AMD AMF SDK - CONFIGURADO Y LISTO
# ============================================================================

set(AMF_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib/AMF)

# Agregar directorios de include de AMF
target_include_directories(${DRIVER_NAME} PRIVATE
    ${AMF_LIB_DIR}
    ${AMF_LIB_DIR}/core
    ${AMF_LIB_DIR}/components
    )

# AMF en Windows es header-only en tiempo de compilación
# Las DLLs se cargan dinámicamente en runtime
message(STATUS "AMD AMF SDK encontrado en: ${AMF_LIB_DIR}")
message(STATUS "  - Headers: core/ y components/")
message(STATUS "  - DLLs se cargarán dinámicamente (amfrt64.dll)")

# ============================================================================
# SDKS DE FABRICANTES (OPCIONAL - Descomentar si tienes instalados)
# ============================================================================

# NVIDIA NVENC SDK (https://developer.nvidia.com/video-encode-sdk)
# Descomentar y ajustar path si tienes NVIDIA Video Codec SDK instalado
# if (WIN32)
#     set(NVIDIA_SDK_PATH "C:/Program Files/NVIDIA/NVIDIA Video Codec SDK")
#     find_library(NVENC_LIB 
#         NAMES nvEncodeAPI64 
#         PATHS "${NVIDIA_SDK_PATH}/Lib/x64"
#     )
#     if (NVENC_LIB)
#         target_link_libraries(${DRIVER_NAME} PRIVATE ${NVENC_LIB})
#         target_include_directories(${DRIVER_NAME} PRIVATE "${NVIDIA_SDK_PATH}/Include")
#         message(STATUS "NVIDIA NVENC SDK encontrado en: ${NVIDIA_SDK_PATH}")
#     else()
#         message(STATUS "NVIDIA NVENC SDK no encontrado - NVENC encoding no disponible")
#     endif()
# endif ()

# Intel oneVPL / Media SDK (https://github.com/intel/libvpl)
# Descomentar y ajustar path si tienes Intel Media SDK instalado
# if (WIN32)
#     set(INTEL_SDK_PATH "C:/Program Files (x86)/Intel/Media SDK")
#     find_library(MFX_LIB 
#         NAMES libmfx 
#         PATHS "${INTEL_SDK_PATH}/lib/x64"
#     )
#     if (MFX_LIB)
#         target_link_libraries(${DRIVER_NAME} PRIVATE ${MFX_LIB})
#         target_include_directories(${DRIVER_NAME} PRIVATE "${INTEL_SDK_PATH}/include")
#         message(STATUS "Intel Media SDK encontrado en: ${INTEL_SDK_PATH}")
#     else()
#         message(STATUS "Intel Media SDK no encontrado - QuickSync encoding no disponible")
#     endif()
# endif ()

# ============================================================================
# COPIAR ARCHIVOS DE RECURSOS
# ============================================================================

add_custom_command(
        TARGET ${DRIVER_NAME}
        PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/${TARGET_NAME}
        ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}
)