cmake_minimum_required(VERSION 3.16)

# vcpkg toolchain. These must be set BEFORE project(): CMake reads the toolchain file inside
# project(), so anything set after it is ignored. The libraries were installed in manifest mode
# from /home/heartslaught/vcpkg, so they live under vcpkg_installed/<triplet>, not installed/<triplet>.
set(CMAKE_TOOLCHAIN_FILE "/home/heartslaught/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE FILEPATH "vcpkg toolchain")
set(VCPKG_TARGET_TRIPLET "x64-linux-release-clang" CACHE STRING "vcpkg triplet")
set(VCPKG_INSTALLED_DIR "/home/heartslaught/vcpkg/vcpkg_installed" CACHE PATH "vcpkg installed tree")

project(ive_poc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(IVEVSG_BUILD_TESTS "Build the Google Test suite" ON)
set(IVEVSG_TEST_IVE "/run/media/heartslaught/games/ive_test/gadas-world.ive" CACHE FILEPATH
    "Paged .ive database used by the loader integration test (the test is skipped if it is missing)")
set(IVEVSG_DEFAULT_TILES "/run/media/heartslaught/games/ive_test/bluemarble_tiles_l11/root.ive" CACHE FILEPATH
    "Tile pyramid root the viewer uses as its base map by default (falls back to the single image if missing)")

find_package(vsg CONFIG REQUIRED)
# Note the capital X: the config file is vsgXchangeConfig.cmake, and Linux is case-sensitive.
find_package(vsgXchange CONFIG REQUIRED)

# Sanity check: was the vsgXchange we found built with OSG (osg2vsg) support?
# osg2vsg is a PRIVATE dependency of the shared library, so it never shows up in
# INTERFACE_LINK_LIBRARIES. The reliable signal is the generated Version.h.
get_target_property(_xchg_includes vsgXchange::vsgXchange INTERFACE_INCLUDE_DIRECTORIES)
set(_xchg_osg OFF)
foreach(_dir IN LISTS _xchg_includes)
    if(EXISTS "${_dir}/vsgXchange/Version.h")
        file(STRINGS "${_dir}/vsgXchange/Version.h" _osg_line REGEX "^[ \t]*#define[ \t]+vsgXchange_OSG")
        if(_osg_line)
            set(_xchg_osg ON)
        endif()
    endif()
endforeach()
if(_xchg_osg)
    message(STATUS "vsgXchange OSG support: ON (.ive loading available)")
else()
    message(WARNING "vsgXchange OSG support: OFF -- .ive loading is NOT available. "
                    "Rebuild vsgxchange with the 'osg' feature enabled.")
endif()
set(IVEVSG_HAS_OSG ${_xchg_osg})

add_subdirectory(lib)
add_subdirectory(app)
add_subdirectory(tools/ivetiler)

if(IVEVSG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "OSG plugin directory (Debug):   ${IVEVSG_OSG_PLUGINS_DEBUG}")
message(STATUS "OSG plugin directory (Release): ${IVEVSG_OSG_PLUGINS_RELEASE}")
