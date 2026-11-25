# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)
project(agbsf2 VERSION 1.12.0 LANGUAGES CXX)

# The name of the macOS droplet app.
set(AGBSF2_APP_NAME Agbsf2)

# xsflib is a library for players and rippers of the PSF family. It's a submodule. Its XsflibProject.cmake sets what a
# project built on it starts with: C++20, a release build by default, the static C runtime, the options that keep the
# output the same on every platform, and the warnings for agbsf2's targets (AGBSF2_WARNINGS). It refuses the compilers
# whose output would differ from other builds.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/externals/xsflib/CMakeLists.txt")
    message(FATAL_ERROR "externals/xsflib is missing. Run: git submodule update --init")
endif()
include(externals/xsflib/cmake/XsflibProject.cmake)
xsflib_project(CHECK_COMPILERS NAME agbsf2 PREFIX AGBSF2)

option(AGBSF2_BUILD_TESTS "Build and register the tests" ON)

# seqlib writes the MIDI files and SoundFonts. It comes first. xsflib then uses it in place of the copy in the xsflib
# submodule. agbsf2 uses the console code and the common files of xsflib. The GBA module of agblib uses the xsflib PSF
# container. That container decompresses the programs without zlib. ntdlib is the Nintendo console library that agblib
# builds on. agblib is the GBA sound library. It contains the ROM and GSF reader and all fourteen sound drivers, with
# their conversions.
set(NTDLIB_SEQUENCE_EXPORT ON)
set(AGBLIB_SEQUENCE_EXPORT ON)
xsflib_add_libraries(seqlib xsflib ntdlib agblib)

add_library(agbsf2_core STATIC
    src/music.cpp
    src/brownie/music.cpp
    src/konami/music.cpp
    src/krawall/music.cpp
    src/mp2k/music.cpp
    src/quintet/music.cpp
    src/rare/music.cpp
    src/rd2/music.cpp
    src/rd1/music.cpp
    src/ubimilan/music.cpp
    src/alphadream/music.cpp
    src/logikstate/music.cpp
    src/webfoot/music.cpp
    src/frontline_mod/music.cpp
    src/frontline_xm/music.cpp
    src/engine_software/music.cpp
)
target_include_directories(agbsf2_core PUBLIC src)
target_link_libraries(agbsf2_core PUBLIC seqlib::seqlib agblib::sequence_export_mp2k agblib::sequence_export_konami
                      agblib::sequence_export_krawall agblib::sequence_export_quintet agblib::sequence_export_rare
                      agblib::sequence_export_rd2 agblib::sequence_export_brownie agblib::sequence_export_ubimilan
                      agblib::sequence_export_rd1 agblib::sequence_export_alphadream
                      agblib::sequence_export_logikstate agblib::sequence_export_webfoot
                      agblib::sequence_export_frontline_mod agblib::sequence_export_frontline_xm
                      agblib::sequence_export_engine_software)

# The warnings of agbsf2's targets. xsflib_project's options keep the floats the same on every platform.
if(MSVC)
    target_compile_options(agbsf2_core PUBLIC ${AGBSF2_WARNINGS} /utf-8)
else()
    target_compile_options(agbsf2_core PUBLIC ${AGBSF2_WARNINGS} -Wpedantic)
endif()

add_executable(agbsf2 src/main.cpp)
target_link_libraries(agbsf2 PRIVATE agbsf2_core xsflib::console)
# The version, and on Windows the version and description that Explorer shows in the file's properties.
xsflib_add_version(agbsf2 VERSION ${PROJECT_VERSION} DEFINE AGBSF2_VERSION
                   DESCRIPTION "MIDI and SoundFont converter for GBA game music" PRODUCT agbsf2
                   COPYRIGHT "Copyright (c) 2026 the agbsf2 authors. MIT License.")
# Windows: UTF-8 as the code page. An archive or a folder whose name the system's code page can't represent then opens.
xsflib_add_utf8_manifest(agbsf2)
include(GNUInstallDirs)
install(TARGETS agbsf2 RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES LICENSE THIRD_PARTY_NOTICES DESTINATION "${CMAKE_INSTALL_DATADIR}/agbsf2")

# macOS: a droplet app that converts the files dropped on it in the Finder. The app contains the licences beside the
# program. Its script takes the handlers that xsflib's droplet scripts share (HANDLERS).
set(AGBSF2_APP "${CMAKE_BINARY_DIR}/${AGBSF2_APP_NAME}.app")
xsflib_add_droplet(agbsf2 VERSION ${PROJECT_VERSION} APP_NAME ${AGBSF2_APP_NAME} HANDLERS
                   SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/${AGBSF2_APP_NAME}.applescript"
                   RESOURCES LICENSE THIRD_PARTY_NOTICES)

if(AGBSF2_BUILD_TESTS)
    enable_testing()
    add_executable(agbsf2_tests tests/test_main.cpp tests/konami_test.cpp tests/rare_test.cpp tests/quintet_test.cpp
                                tests/mp2k_test.cpp tests/rd2_test.cpp tests/brownie_test.cpp tests/krawall_test.cpp
                                tests/ubimilan_test.cpp tests/logikstate_test.cpp tests/webfoot_test.cpp
                                tests/frontline_mod_test.cpp tests/frontline_xm_test.cpp
                                tests/engine_software_test.cpp)
    target_link_libraries(agbsf2_tests PRIVATE agbsf2_core agblib::testing)
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
