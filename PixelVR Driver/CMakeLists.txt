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

add_library(${DRIVER_NAME} SHARED
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
        )

# ✅ LINKEAR AMBAS
target_link_libraries(${DRIVER_NAME} PRIVATE ${OPENVR_LIBRARIES} ${PSMOVE_LIBRARIES})

# ✅ INCLUDE AMBAS
target_include_directories(${DRIVER_NAME} PRIVATE ${OPENVR_INCLUDE_DIR} ${PSMOVE_INCLUDE_DIR})

# Copy driver assets to output folder
add_custom_command(
        TARGET ${DRIVER_NAME}
        PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/${TARGET_NAME}
        ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}
)