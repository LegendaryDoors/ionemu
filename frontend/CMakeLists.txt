# iONemu, a Hasbro iON emulator: the Qt 6 desktop front end for ioncore, PCSX2-style.
# Build:  cmake -S frontend -B frontend/build -G Ninja && cmake --build frontend/build   -> frontend/build/ionemu
# No SDL here: Qt Widgets for the UI, Qt Multimedia for audio and the webcam. The core is pulled in as a
# subdirectory; its command-line tools and tests stay in ioncore/build.
cmake_minimum_required(VERSION 3.21)
project(ionemu VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Multimedia Network Svg)
# QZipReader (Download and Install) is private Qt API: from Qt 6.9 CorePrivate is a package of its own, before that it
# comes with Core, and Qt 6.5 still keeps the zip reader in QtGui (see updater.cpp).
if(Qt6_VERSION VERSION_GREATER_EQUAL 6.9)
  find_package(Qt6 REQUIRED COMPONENTS CorePrivate)
endif()
qt_standard_project_setup()
set(CMAKE_AUTORCC ON)   # icons.qrc (qt_standard_project_setup enables AUTOMOC/AUTOUIC only)

if(NOT TARGET ioncore)                                   # standalone build; the top-level CMakeLists adds ioncore itself
  set(IONCORE_BUILD_TOOLS OFF CACHE BOOL "ioncore tools are built from ioncore/, not here")
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../ioncore ioncore)
endif()

qt_add_executable(ionemu
  src/main.cpp
  src/app.cpp src/app.h
  src/game_list.cpp src/game_list.h
  src/scanner.cpp src/scanner.h
  src/cover_delegate.cpp src/cover_delegate.h
  src/audio_output.cpp src/audio_output.h
  src/emu_thread.cpp src/emu_thread.h
  src/display_widget.cpp src/display_widget.h
  src/main_window.cpp src/main_window.h
  src/settings.cpp src/settings.h
  src/settings_dialog.cpp src/settings_dialog.h
  src/camera_source.cpp src/camera_source.h
  src/camera_preview.cpp src/camera_preview.h
  src/cover_generator.cpp src/cover_generator.h
  src/cover_download.cpp src/cover_download.h
  src/icons.cpp src/icons.h
  src/updater.cpp src/updater.h
  icons.qrc
)
target_link_libraries(ionemu PRIVATE ioncore Qt6::Widgets Qt6::Multimedia Qt6::Network Qt6::Svg Qt6::CorePrivate)
if(Qt6_VERSION VERSION_LESS 6.9)
  target_link_libraries(ionemu PRIVATE Qt6::GuiPrivate)
endif()
target_compile_options(ionemu PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wno-unused-parameter>)
# The bundle's minimum macOS follows the deployment target (release/build_macos.sh passes 13.0, the floor of the Qt
# 6.10 binaries); a plain macOS build without one gets 12.0.
if(CMAKE_OSX_DEPLOYMENT_TARGET)
  set(IONEMU_MACOS_MINIMUM "${CMAKE_OSX_DEPLOYMENT_TARGET}")
else()
  set(IONEMU_MACOS_MINIMUM "12.0")
endif()
set_target_properties(ionemu PROPERTIES
  WIN32_EXECUTABLE TRUE
  qt_no_entrypoint TRUE   # main.cpp provides WinMain: Qt's MinGW entry-point library expects the msvcrt runtime, a UCRT cross toolchain has no __imp___argc
  MACOSX_BUNDLE TRUE
  MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in   # NSCameraUsageDescription: macOS kills apps that open a camera without it
  MACOSX_BUNDLE_GUI_IDENTIFIER org.ionemu.ionemu
  MACOSX_BUNDLE_BUNDLE_NAME "iONemu"
  MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
  MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
  MACOSX_BUNDLE_ICON_FILE ionemu.icns)
# File icons: Explorer and Finder read them from the program file itself (the window icon only exists once the app
# runs). On Windows the resource also carries the version info that Task Manager and the Details tab show.
if(WIN32)
  configure_file(ionemu.rc.in ${CMAKE_CURRENT_BINARY_DIR}/ionemu.rc @ONLY)
  target_sources(ionemu PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/ionemu.rc)
elseif(APPLE)
  target_sources(ionemu PRIVATE branding/ionemu.icns)
  set_source_files_properties(branding/ionemu.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endif()
