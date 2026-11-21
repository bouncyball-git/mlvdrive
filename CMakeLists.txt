cmake_minimum_required(VERSION 3.18)
project(MLVDrive VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
# The C runtime linked in: the programs need no Visual C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
    set(_warn_off /W1 /wd4068 /wd4244 /wd4267 /wd4305 /wd4018)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
    add_compile_options(/utf-8 /MP)
endif()

# --- MLV-App's portable C code (third_party/mlvapp): lossless JPEG, camera table, pixel fixes ---
set(MLVAPP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mlvapp")
add_library(mlvapp_c STATIC
    ${MLVAPP_DIR}/liblj92/lj92.c
    ${MLVAPP_DIR}/camid/camera_id.c
    ${MLVAPP_DIR}/llrawproc/pixelproc.c)
target_include_directories(mlvapp_c PUBLIC ${MLVAPP_DIR})
target_compile_definitions(mlvapp_c PRIVATE STDOUT_SILENT)
if(MSVC)
    target_compile_options(mlvapp_c PRIVATE ${_warn_off})
else()
    target_compile_options(mlvapp_c PRIVATE -w)
    target_link_libraries(mlvapp_c PRIVATE m)
endif()

# --- MLV reading and decoding, raw corrections, white balance and camera matrices, thread pool,
#     DNG writer ---
add_library(mlv_core STATIC
    src/util/file.cpp
    src/mlv/clip.cpp
    src/mlv/color.cpp
    src/raw/rawfix.cpp
    src/parallel.cpp
    src/dng/dng_writer.cpp
    src/mlv/wav.cpp)
target_include_directories(mlv_core PUBLIC src)
find_package(Threads REQUIRED)
target_link_libraries(mlv_core PUBLIC mlvapp_c Threads::Threads)
if(MSVC)
    target_compile_options(mlv_core PRIVATE /W3 /O2)
else()
    target_compile_options(mlv_core PRIVATE -Wall -Wextra -Wno-unused-parameter -O3)
endif()

# --- The drive's contents: clips found under a folder, as folders of DNG files ---
add_library(mlv_vfs STATIC src/vfs/tree.cpp src/vfs/frame_cache.cpp src/vfs/test_patterns.cpp)
target_link_libraries(mlv_vfs PUBLIC mlv_core)
if(MSVC)
    target_compile_options(mlv_vfs PRIVATE /W3 /O2)
endif()

# --- mlvdrive: the drive (Windows: WinFsp, its DLL loaded from WinFsp's install folder) ---
if(WIN32)
    get_filename_component(_winfsp_reg "[HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\WinFsp;InstallDir]" ABSOLUTE)
    if(EXISTS "${_winfsp_reg}/inc/winfsp/winfsp.h")
        set(_winfsp_default "${_winfsp_reg}")
    else()
        set(_winfsp_default "C:/Program Files (x86)/WinFsp")
    endif()
    set(WINFSP_DIR "${_winfsp_default}" CACHE PATH "WinFsp install folder (with the Developer feature)")
    if(NOT EXISTS "${WINFSP_DIR}/inc/winfsp/winfsp.h")
        message(FATAL_ERROR "WinFsp SDK not found in ${WINFSP_DIR}: install WinFsp with the Developer feature "
                            "(https://winfsp.dev/rel/) or set -DWINFSP_DIR=")
    endif()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64|aarch64")
        set(_winfsp_arch a64)
    else()
        set(_winfsp_arch x64)
    endif()

    add_executable(mlvdrive src/main.cpp src/fs/winfsp_fs.cpp)
    target_include_directories(mlvdrive PRIVATE "${WINFSP_DIR}/inc")
    target_link_libraries(mlvdrive PRIVATE mlv_vfs "${WINFSP_DIR}/lib/winfsp-${_winfsp_arch}.lib" delayimp advapi32)
    target_link_options(mlvdrive PRIVATE "/DELAYLOAD:winfsp-${_winfsp_arch}.dll")
    target_compile_definitions(mlvdrive PRIVATE UNICODE _UNICODE)
    target_compile_options(mlvdrive PRIVATE /W3)
endif()

# --- mlvdrive_cli: clip info, one DNG to disk, timings ---
add_executable(mlvdrive_cli tools/cli.cpp)
target_link_libraries(mlvdrive_cli PRIVATE mlv_vfs)
if(MSVC)
    target_compile_options(mlvdrive_cli PRIVATE /W3)
endif()
