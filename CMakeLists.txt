# iONemu: a Hasbro iON emulator. Builds the core (ioncore/, with its tools and tests) and the launcher (frontend/).
cmake_minimum_required(VERSION 3.21)
project(ionemu VERSION 0.1.0 LANGUAGES CXX)

option(IONEMU_BUILD_FRONTEND "build the Qt 6 launcher" ON)
set(IONCORE_BUILD_TOOLS ON CACHE BOOL "build ionrun, ionrepair, ionview and the core tests")
add_subdirectory(ioncore)
if(IONEMU_BUILD_FRONTEND)
  add_subdirectory(frontend)
endif()
