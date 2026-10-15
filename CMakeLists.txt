# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)
project(agbsf2 VERSION 1.6.1 LANGUAGES CXX)

# The name of the macOS droplet app.
set(AGBSF2_APP_NAME Agbsf2)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Link MSVC's runtime statically, so the released .exe runs without the runtime DLLs installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(AGBSF2_BUILD_TESTS "Build and register the tests" ON)

# seqlib, the library that writes the MIDI files and SoundFonts, is a submodule.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/externals/seqlib/CMakeLists.txt")
    message(FATAL_ERROR "externals/seqlib is missing. Run: git submodule update --init")
endif()
add_subdirectory(externals/seqlib EXCLUDE_FROM_ALL)

# xsflib, a library for players and rippers of the PSF formats, is a submodule too. agbsf2 takes xsflib's PSF container,
# console code and common files. The container doesn't require zlib: agbsf2 decompresses the programs itself
# (src/inflate.cpp).
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/externals/xsflib/CMakeLists.txt")
    message(FATAL_ERROR "externals/xsflib is missing. Run: git submodule update --init")
endif()
add_subdirectory(externals/xsflib EXCLUDE_FROM_ALL)

add_library(agbsf2_core STATIC
    src/beat_grid.cpp
    src/inflate.cpp
    src/music.cpp
    src/rom.cpp
    src/sf2.cpp
    src/song_banks.cpp
    src/brownie/convert.cpp
    src/brownie/driver.cpp
    src/brownie/music.cpp
    src/brownie/sequencer.cpp
    src/brownie/song.cpp
    src/brownie/soundfont.cpp
    src/konami/convert.cpp
    src/konami/driver.cpp
    src/konami/music.cpp
    src/konami/pitch.cpp
    src/konami/seqformat.cpp
    src/konami/sequencer.cpp
    src/konami/soundfont.cpp
    src/krawall/convert.cpp
    src/krawall/driver.cpp
    src/krawall/music.cpp
    src/krawall/player.cpp
    src/krawall/song.cpp
    src/mp2k/convert.cpp
    src/mp2k/driver.cpp
    src/mp2k/music.cpp
    src/mp2k/sequencer.cpp
    src/mp2k/song.cpp
    src/mp2k/soundfont.cpp
    src/rare/convert.cpp
    src/rare/driver.cpp
    src/rare/music.cpp
    src/rare/sequencer.cpp
    src/rare/song.cpp
    src/rare/soundfont.cpp
    src/quintet/convert.cpp
    src/quintet/driver.cpp
    src/quintet/music.cpp
    src/quintet/sequencer.cpp
    src/quintet/song.cpp
    src/quintet/soundfont.cpp
    src/rd2/convert.cpp
    src/rd2/driver.cpp
    src/rd2/music.cpp
    src/rd2/sequencer.cpp
    src/rd2/song.cpp
    src/rd2/soundfont.cpp
    src/ubimilan/convert.cpp
    src/ubimilan/driver.cpp
    src/ubimilan/music.cpp
    src/ubimilan/sequencer.cpp
    src/ubimilan/soundfont.cpp
    src/thumb.cpp
)
target_include_directories(agbsf2_core PUBLIC src)
target_link_libraries(agbsf2_core PUBLIC seqlib::seqlib xsflib::psf_container)

# Disable fused multiply-add so output stays byte-identical across platforms. GCC and Clang can fuse a * b + c on
# CPUs such as ARM64, changing the rounding. MSVC's default /fp:precise disables this in Visual Studio 2022 and later;
# clang-cl needs an explicit option.
if(MSVC)
    target_compile_options(agbsf2_core PUBLIC /W4 /utf-8)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(agbsf2_core PUBLIC /clang:-ffp-contract=off)
    endif()
else()
    target_compile_options(agbsf2_core PUBLIC -Wall -Wextra -Wpedantic -ffp-contract=off)
endif()

add_executable(agbsf2 src/main.cpp)
target_link_libraries(agbsf2 PRIVATE agbsf2_core xsflib::console)
# The version, and on Windows the version and description that Explorer shows in the file's properties.
xsflib_add_version(agbsf2 VERSION ${PROJECT_VERSION} DEFINE AGBSF2_VERSION
                   DESCRIPTION "MIDI and SoundFont converter for GBA game music" PRODUCT agbsf2
                   COPYRIGHT "Copyright (c) 2026 the agbsf2 authors. MIT License.")
include(GNUInstallDirs)
install(TARGETS agbsf2 RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES LICENSE THIRD_PARTY_NOTICES DESTINATION "${CMAKE_INSTALL_DATADIR}/agbsf2")

# macOS: a droplet app that converts the files dropped on it in the Finder. The app holds the licences beside the
# program.
set(AGBSF2_APP "${CMAKE_BINARY_DIR}/${AGBSF2_APP_NAME}.app")
xsflib_add_droplet(agbsf2 VERSION ${PROJECT_VERSION} APP_NAME ${AGBSF2_APP_NAME}
                   SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/${AGBSF2_APP_NAME}.applescript"
                   RESOURCES LICENSE THIRD_PARTY_NOTICES)

if(AGBSF2_BUILD_TESTS)
    enable_testing()
    add_executable(agbsf2_tests tests/test_main.cpp tests/test_util.cpp tests/konami_test.cpp tests/rare_test.cpp
                                tests/quintet_test.cpp tests/mp2k_test.cpp tests/rd2_test.cpp
                                tests/brownie_test.cpp tests/krawall_test.cpp tests/ubimilan_test.cpp)
    target_link_libraries(agbsf2_tests PRIVATE agbsf2_core seqlib::testing)
    add_test(NAME agbsf2_tests COMMAND agbsf2_tests)

    # Test the droplet app's conversion report using a stand-in for the program.
    find_program(OSASCRIPT osascript)
    if(TARGET agbsf2_app AND OSASCRIPT)
        add_test(NAME droplet
                 COMMAND "${OSASCRIPT}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/droplet_test.applescript"
                         "${AGBSF2_APP}/Contents/Resources/Scripts/main.scpt"
                         "${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_tool.sh")
    endif()

    # Load local_tests.cmake from each immediate subdirectory to register tests that use locally supplied files.
    xsflib_include_local_tests()
endif()
