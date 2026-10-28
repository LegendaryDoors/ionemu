#!/usr/bin/env bash
# Build iONemu for macOS on a Mac with the Qt binaries, then wrap the bundle in a .dmg.
#   usage: release/build_macos.sh SRC OUT QTDIR
#   SRC   the source tree (this repo)
#   OUT   build directory; the result is OUT/ionemu-<version>-macos.dmg and OUT/dist/iONemu.app
#   QTDIR the Qt for macOS prefix, e.g. ~/Qt/6.10.1/macos (aqt: install-qt mac desktop 6.10.1 clang_64 -m qtmultimedia)
# The app is ad-hoc signed, not with an Apple certificate: users allow it once in System Settings > Privacy & Security.
set -euo pipefail
SRC="${1:?source tree}"; OUT="${2:?build dir}"; QTDIR="${3:?Qt prefix}"
VERSION="$(sed -n 's/.*kVersion = "\([^"]*\)".*/\1/p' "$SRC/frontend/src/app.h")"
cmake -S "$SRC" -B "$OUT" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QTDIR" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 -DCMAKE_OSX_ARCHITECTURES="${ARCHS:-x86_64;arm64}" -DIONCORE_BUILD_TOOLS=ON \
  -DCMAKE_DISABLE_FIND_PACKAGE_SDL3=ON   # universal by default; ionview (SDL3) is not shipped and a Homebrew SDL3 is single-arch
cmake --build "$OUT"
APP="$(find "$OUT/frontend" -maxdepth 1 -name '*.app' | head -n1)"
[ -n "$APP" ] || { echo "no .app bundle under $OUT/frontend"; exit 1; }
DIST="$OUT/dist"; [ ! -e "$DIST" ] || { echo "$DIST exists: pass a fresh build folder (this script never deletes)"; exit 1; }; mkdir -p "$DIST"
cp -R "$APP" "$DIST/iONemu.app"           # the bundle carries the product name, not the CMake target name
APP="$DIST/iONemu.app"
"$QTDIR/bin/macdeployqt" "$APP" -always-overwrite
cp "$OUT/ioncore/ionrun" "$OUT/ioncore/ionrepair" "$DIST/"
# Apple Silicon runs only code with a valid signature, and macdeployqt's rewrites break the ones the linker and Qt
# made: ad-hoc sign everything again (no Apple certificate needed; Gatekeeper still asks once), then verify.
codesign --force --deep --sign - "$APP"
codesign --force --sign - "$DIST/ionrun" "$DIST/ionrepair"
for arch in x86_64 arm64; do codesign --verify --deep --strict --architecture "$arch" "$APP"; done
codesign --verify --strict "$DIST/ionrun" "$DIST/ionrepair"
cp "$SRC/LICENSE" "$DIST/LICENSE.txt" 2>/dev/null || cp "$SRC/frontend/licenses/GPL-3.0.txt" "$DIST/LICENSE.txt"
cp "$SRC/frontend/licenses/THIRD_PARTY.txt" "$SRC/frontend/licenses/LGPL-3.0.txt" "$DIST/"   # Qt is LGPL: its text travels with the frameworks
cp "$SRC/README.md" "$DIST/README.md"
ln -sfn /Applications "$DIST/Applications"
DMG="$OUT/ionemu-$VERSION-macos.dmg"
hdiutil create -volname "iONemu $VERSION" -srcfolder "$DIST" -ov -format UDZO "$DMG"
ZIP="$OUT/ionemu-$VERSION-macos.zip"; (cd "$DIST" && ditto -c -k --keepParent iONemu.app "$ZIP")   # what Download and Install fetches (symlinks kept)
echo "updater package: $ZIP"; ls -la "$ZIP"
echo "macOS build: $DMG"; ls -la "$DMG"; lipo -archs "$APP/Contents/MacOS/"* 2>/dev/null || true
