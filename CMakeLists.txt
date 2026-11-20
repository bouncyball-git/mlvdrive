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

# --- MLV reading and decoding, raw corrections, white balance and camera matrices, thread pool ---
add_library(mlv_core STATIC
    src/util/file.cpp
    src/mlv/clip.cpp
    src/mlv/color.cpp
    src/raw/rawfix.cpp
    src/parallel.cpp)
target_include_directories(mlv_core PUBLIC src)
find_package(Threads REQUIRED)
target_link_libraries(mlv_core PUBLIC mlvapp_c Threads::Threads)
if(MSVC)
    target_compile_options(mlv_core PRIVATE /W3 /O2)
else()
    target_compile_options(mlv_core PRIVATE -Wall -Wextra -Wno-unused-parameter -O3)
endif()

# --- mlvdrive_cli: clip info, one DNG to disk, timings ---
add_executable(mlvdrive_cli tools/cli.cpp)
target_link_libraries(mlvdrive_cli PRIVATE mlv_core)
if(MSVC)
    target_compile_options(mlvdrive_cli PRIVATE /W3)
endif()
