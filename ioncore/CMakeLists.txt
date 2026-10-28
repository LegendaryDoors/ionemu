cmake_minimum_required(VERSION 3.21)
project(ioncore LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# The u'nSP core is generated from MAME by src/cpu/port_unsp.py (uses src/cpu/mame_orig/, no network needed).
file(GLOB UNSP_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/cpu/unsp/*.cpp)
if(NOT UNSP_SOURCES)
  message(FATAL_ERROR "src/cpu/unsp/ is empty: run  python3 src/cpu/port_unsp.py")
endif()

# Vendored/ported MAME code (GPL-2.0+ CPU, BSD-3 renderer) builds without warnings; our code builds with them.
set(VENDORED_SOURCES ${UNSP_SOURCES} src/ppu/spg_renderer.cpp)
set_source_files_properties(${VENDORED_SOURCES} PROPERTIES COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-w>")

add_library(ioncore STATIC
  ${VENDORED_SOURCES}
  src/spu/ion_spu.cpp
  src/soc.cpp
  src/spi_flash.cpp
  src/disc.cpp
  src/board.cpp
  src/firmware_hle.cpp
  src/core.cpp
)
target_include_directories(ioncore
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/src/cpu/shim ${CMAKE_CURRENT_SOURCE_DIR}/src/cpu/unsp
          ${CMAKE_CURRENT_SOURCE_DIR}/src/ppu ${CMAKE_CURRENT_SOURCE_DIR}/src/spu)
set(IONCORE_WARNINGS $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wno-unused-parameter>)
set_source_files_properties(src/spu/ion_spu.cpp src/soc.cpp src/spi_flash.cpp src/disc.cpp src/board.cpp src/firmware_hle.cpp src/core.cpp
  PROPERTIES COMPILE_OPTIONS "${IONCORE_WARNINGS}")

# The command-line tools and tests build when ioncore is the top-level project; a frontend that adds this directory
# with add_subdirectory() gets only the library unless it sets IONCORE_BUILD_TOOLS.
option(IONCORE_BUILD_TOOLS "build ionrun, ionview and the tests" ${PROJECT_IS_TOP_LEVEL})
if(IONCORE_BUILD_TOOLS)
add_executable(ionrun apps/ionrun.cpp)
target_link_libraries(ionrun PRIVATE ioncore)
add_executable(ionrepair apps/ionrepair.cpp)
target_link_libraries(ionrepair PRIVATE ioncore)

find_package(SDL3 CONFIG QUIET)
if(SDL3_FOUND)
  add_executable(ionview apps/ionview.cpp)
  target_link_libraries(ionview PRIVATE ioncore SDL3::SDL3)
else()
  message(STATUS "SDL3 not found: ionview (the SDL3 viewer) will not be built")
endif()

# Tests. Game content is never stored in the repo: data-driven tests read IONCORE_TEST_EXTRACT (your Blue's Room
# disc, never part of the repository; set it with -DIONCORE_TEST_EXTRACT=...) and skip when it is absent.
# Golden runs: tests/golden.py.
enable_testing()
add_executable(ioncore_instance_test tests/instance_test.cpp)
target_link_libraries(ioncore_instance_test PRIVATE ioncore)
set(IONCORE_TEST_EXTRACT "" CACHE PATH "Blue's Room disc (image or extracted directory) used by the tests")
add_test(NAME instances_and_save_states COMMAND ioncore_instance_test "${IONCORE_TEST_EXTRACT}" 3000)
add_executable(ioncore_camera_test tests/camera_test.cpp)
target_link_libraries(ioncore_camera_test PRIVATE ioncore)
add_test(NAME external_camera_motion COMMAND ioncore_camera_test "${IONCORE_TEST_EXTRACT}" 1500)
endif()  # IONCORE_BUILD_TOOLS
