# Cross-compiling iONemu for Windows from Linux with MinGW-w64 (used through Qt's own qt.toolchain.cmake as the
# chain-loaded toolchain, see release/build_windows.sh).
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(TRIPLE x86_64-w64-mingw32)
set(CMAKE_C_COMPILER ${TRIPLE}-gcc)
set(CMAKE_CXX_COMPILER ${TRIPLE}-g++)
set(CMAKE_RC_COMPILER ${TRIPLE}-windres)
set(CMAKE_FIND_ROOT_PATH /usr/${TRIPLE})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# Link libgcc, libstdc++ and winpthread statically (Qt's DLLs still link through their import libraries), so our
# programs do not depend on the runtime DLLs Qt ships. Plain -static, not --whole-archive: that would also pull in
# winpthread's own version resource, which then clashes with ours (Task Manager would name the program after it).
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
