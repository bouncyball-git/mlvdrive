cmake_minimum_required(VERSION 3.18)
# macOS: 26 or later (FSKit's folder-backed volumes); set before project() for every target.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0" CACHE STRING "Minimum macOS version")
endif()
project(MLVDrive VERSION 0.9.0 LANGUAGES C CXX)
# The version everywhere: the programs, the DNG Software tag, the tray app's resources and the
# release zip's name (build.ps1 reads it from the line above).
add_compile_definitions(MLVDRIVE_VERSION="${PROJECT_VERSION}")

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

# --- MLV reading and decoding, raw corrections, white balance, the DNG and WAV writers ---
add_library(mlv_core STATIC
    src/util/file.cpp
    src/mlv/clip.cpp
    src/mlv/color.cpp
    src/raw/rawfix.cpp
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

# --- mlvdrive: the drive (Windows: WinFsp, its DLL loaded from WinFsp's install folder; macOS:
# an FSKit module in MLV Drive.app) ---
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

    # The drive, the settings file and mounting: shared by the console program and the tray app.
    add_library(mlvdrive_app STATIC src/fs/winfsp_fs.cpp src/app/settings.cpp src/app/mounter.cpp)
    target_include_directories(mlvdrive_app PRIVATE "${WINFSP_DIR}/inc")
    target_compile_definitions(mlvdrive_app PUBLIC UNICODE _UNICODE)
    target_compile_options(mlvdrive_app PRIVATE /W3)
    target_link_libraries(mlvdrive_app PUBLIC mlv_vfs "${WINFSP_DIR}/lib/winfsp-${_winfsp_arch}.lib" delayimp
                                              advapi32 shell32 ole32)
    target_link_options(mlvdrive_app INTERFACE "/DELAYLOAD:winfsp-${_winfsp_arch}.dll")

    # mlvdrive: the console program
    add_executable(mlvdrive src/main.cpp)
    target_link_libraries(mlvdrive PRIVATE mlvdrive_app)
    target_compile_options(mlvdrive PRIVATE /W3)

    # mlvdrive_tray: the tray app (no console), its icons, manifest and version from tray.rc
    set(MLVDRIVE_ICON_ON "${CMAKE_CURRENT_SOURCE_DIR}/assets/mlvdrive.ico")
    set(MLVDRIVE_ICON_OFF "${CMAKE_CURRENT_SOURCE_DIR}/assets/mlvdrive_off.ico")
    set(MLVDRIVE_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/src/tray/tray.manifest")
    configure_file(src/tray/tray.rc.in "${CMAKE_CURRENT_BINARY_DIR}/tray.rc" @ONLY)
    # The resource compiler's inputs: CMake doesn't see them, so a changed icon wouldn't rebuild it.
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/tray.rc" PROPERTIES
        OBJECT_DEPENDS "${MLVDRIVE_ICON_ON};${MLVDRIVE_ICON_OFF};${MLVDRIVE_MANIFEST}")
    add_executable(mlvdrive_tray WIN32 src/tray/tray.cpp "${CMAKE_CURRENT_BINARY_DIR}/tray.rc")
    target_link_libraries(mlvdrive_tray PRIVATE mlvdrive_app comctl32)
    target_link_options(mlvdrive_tray PRIVATE /MANIFEST:NO)  # tray.rc has it
    target_compile_options(mlvdrive_tray PRIVATE /W3)
elseif(APPLE)
    enable_language(OBJCXX)
    # The FSKit module: the drive's engine runs in it (an app extension, MLVDriveFS.appex).
    add_executable(MLVDriveFS src/fs/fskit_fs.mm src/app/settings.cpp src/app/folder_watcher_mac.cpp)
    target_compile_options(MLVDriveFS PRIVATE -fobjc-arc -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(MLVDriveFS PRIVATE mlv_vfs "-framework Foundation" "-framework FSKit"
                                             "-framework CoreServices")

    # MLV Drive.app: the menu bar app (mounts, settings, setup), carrying the module.
    add_executable(mlvdrive_menubar src/macos/menubar.mm src/app/settings.cpp)
    set_target_properties(mlvdrive_menubar PROPERTIES OUTPUT_NAME "MLV Drive")
    target_compile_options(mlvdrive_menubar PRIVATE -fobjc-arc -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(mlvdrive_menubar PRIVATE mlv_vfs "-framework AppKit" "-framework ServiceManagement"
                                                   "-framework UserNotifications")

    # mlvdrive: mounts, unmounts and rescans from the command line (in MLV Drive.app/Contents/MacOS).
    add_executable(mlvdrive src/macos/mlvdrive_cli.cpp src/app/settings.cpp)
    target_compile_options(mlvdrive PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_link_libraries(mlvdrive PRIVATE mlv_vfs)

    # Assembled, signed (ad hoc, or $SIGN_IDENTITY) and registered after every build; a release
    # build (tools/package-macos.sh) isn't registered on this Mac.
    option(MLVDRIVE_RELEASE_BUNDLE "Assemble MLV Drive.app without registering it here (packaging)" OFF)
    if(MLVDRIVE_RELEASE_BUNDLE)
        set(_bundle_mode "--release")
    else()
        set(_bundle_mode "")
    endif()
    add_custom_target(mlvdrive_bundle ALL
        COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/src/macos/bundle.sh" "${CMAKE_BINARY_DIR}" "${PROJECT_VERSION}" ${_bundle_mode}
        DEPENDS MLVDriveFS mlvdrive_menubar mlvdrive
        COMMENT "Assembling MLV Drive.app")
endif()

# --- mlvdrive_tool: clip info, one DNG to disk, timings ---
add_executable(mlvdrive_tool tools/mlvdrive_tool.cpp)
target_link_libraries(mlvdrive_tool PRIVATE mlv_vfs)
if(MSVC)
    target_compile_options(mlvdrive_tool PRIVATE /W3)
endif()
