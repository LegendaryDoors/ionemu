#!/usr/bin/env bash
# Cross-build iONemu for Windows (x86_64) from Linux with MinGW-w64 and the Qt MinGW binaries fetched by aqtinstall:
#   aqt install-qt windows desktop 6.10.1 win64_mingw -m qtmultimedia -O qt
#   aqt install-qt linux   desktop 6.10.1 linux_gcc_64 -m qtmultimedia -O qt   (host tools)
# usage: release/build_windows.sh [source tree = this repo] [out = build-win]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${1:-$ROOT}"
OUT="${2:-$ROOT/build-win}"
QT_VER="${QT_VER:-6.10.1}"
QT_ROOT="${QT_ROOT:-$ROOT/qt}"                          # where aqt put the Qt trees (any folder)
QT_TARGET="${QT_TARGET:-$QT_ROOT/$QT_VER/mingw_64}"
QT_HOST="${QT_HOST:-$QT_ROOT/$QT_VER/gcc_64}"
[ -d "$QT_TARGET" ] || { echo "missing $QT_TARGET (aqt install-qt windows desktop $QT_VER win64_mingw -m qtmultimedia)"; exit 1; }
[ -d "$QT_HOST" ] || { echo "missing $QT_HOST (aqt install-qt linux desktop $QT_VER linux_gcc_64 -m qtmultimedia)"; exit 1; }
cmake -S "$SRC" -B "$OUT" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$QT_TARGET/lib/cmake/Qt6/qt.toolchain.cmake" \
  -DQT_CHAINLOAD_TOOLCHAIN_FILE="$ROOT/release/toolchain-mingw64.cmake" \
  -DQT_HOST_PATH="$QT_HOST" \
  -DIONCORE_BUILD_TOOLS=ON
cmake --build "$OUT"
# Package: the exe with the Qt DLLs and plugins it needs (a manual windeployqt; windeployqt.exe would need Wine)
DIST="$OUT/dist"
[ ! -e "$DIST" ] || { echo "$DIST exists: pass a fresh output folder (this script never deletes)"; exit 1; }
mkdir -p "$DIST/platforms" "$DIST/imageformats" "$DIST/multimedia" "$DIST/tls" "$DIST/iconengines" "$DIST/styles"
cp "$OUT/frontend/ionemu.exe" "$OUT/ioncore/ionrun.exe" "$OUT/ioncore/ionrepair.exe" "$DIST/"
for dll in Qt6Core Qt6Gui Qt6Widgets Qt6Network Qt6Svg Qt6Multimedia Qt6MultimediaWidgets; do cp "$QT_TARGET/bin/$dll.dll" "$DIST/"; done
# Qt's DLLs need the MinGW runtime they were built with (ours is linked statically into the exe)
for dll in libgcc_s_seh-1 libstdc++-6 libwinpthread-1; do cp "$QT_TARGET/bin/$dll.dll" "$DIST/"; done
cp "$QT_TARGET/bin/"{avcodec,avformat,avutil,swresample,swscale}-*.dll "$DIST/" 2>/dev/null || true   # Qt Multimedia's FFmpeg backend
cp "$QT_TARGET/plugins/platforms/qwindows.dll" "$DIST/platforms/"
cp "$QT_TARGET/plugins/imageformats/"{qjpeg,qsvg}.dll "$DIST/imageformats/" 2>/dev/null || true
cp "$QT_TARGET/plugins/iconengines/qsvgicon.dll" "$DIST/iconengines/" 2>/dev/null || true
cp "$QT_TARGET/plugins/multimedia/"*.dll "$DIST/multimedia/" 2>/dev/null || true
cp "$QT_TARGET/plugins/tls/"*.dll "$DIST/tls/" 2>/dev/null || true
cp "$QT_TARGET/plugins/styles/"*.dll "$DIST/styles/" 2>/dev/null || true
cp "$SRC/LICENSE" "$DIST/" 2>/dev/null || cp "$SRC/frontend/licenses/GPL-3.0.txt" "$DIST/LICENSE"
cp "$SRC/frontend/licenses/THIRD_PARTY.txt" "$SRC/frontend/licenses/LGPL-3.0.txt" "$DIST/"   # Qt is LGPL: its text travels with the DLLs
echo "Windows build in $DIST"; du -sh "$DIST"
