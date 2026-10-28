#!/usr/bin/env bash
# Build the Linux release: an AppImage of the launcher (with ionrun and ionrepair beside it in usr/bin), compiled in
# an Ubuntu 22.04 container so it runs on any distribution with glibc 2.35 or newer (Ubuntu 22.04, Debian 12,
# Fedora 36 and later), against the Qt 6.10.1 Linux binaries from aqtinstall:
#   aqt install-qt linux desktop 6.10.1 linux_gcc_64 -m qtmultimedia -O <QT_ROOT>
#   usage: release/build_linux_appimage.sh [SRC = this repo] [OUT = build-appimage]
# Needs: docker (daemon running, the user allowed to use it), the Qt tree, and linuxdeploy + its Qt plugin
# (https://github.com/linuxdeploy, the "continuous" x86_64 AppImages) in LINUXDEPLOY_DIR, ideally with the AppImage
# runtime next to them (https://github.com/AppImage/type2-runtime/releases, runtime-x86_64); otherwise it is downloaded.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$(cd "${1:-$ROOT}" && pwd)"
OUT="${2:-$ROOT/build-appimage}"; mkdir -p "$OUT"; OUT="$(cd "$OUT" && pwd)"
QT_VER="${QT_VER:-6.10.1}"
QT_ROOT="${QT_ROOT:-$ROOT/qt}"
QT_LINUX="${QT_LINUX:-$QT_ROOT/$QT_VER/gcc_64}"
LINUXDEPLOY_DIR="${LINUXDEPLOY_DIR:-$ROOT/linuxdeploy}"
[ -f "$QT_LINUX/bin/qmake" ] || { echo "missing Qt for Linux at $QT_LINUX"; exit 1; }
[ -f "$LINUXDEPLOY_DIR/linuxdeploy-x86_64.AppImage" ] && [ -f "$LINUXDEPLOY_DIR/linuxdeploy-plugin-qt-x86_64.AppImage" ] || { echo "missing linuxdeploy AppImages in $LINUXDEPLOY_DIR"; exit 1; }
docker run --rm -i -v "$SRC:/src:ro" -v "$QT_LINUX:/qt:ro" -v "$LINUXDEPLOY_DIR:/ld:ro" -v "$OUT:/out" \
  -e DEBIAN_FRONTEND=noninteractive -e OWNER="$(id -u):$(id -g)" ubuntu:22.04 bash -s <<'INNER'
set -euo pipefail
apt-get update -qq >/dev/null
apt-get install -y -qq --no-install-recommends g++-12 cmake ninja-build file patchelf ca-certificates \
  libgl1-mesa-dev libegl1-mesa-dev libxkbcommon-dev libxkbcommon-x11-0 libfontconfig1 libfreetype6 libglib2.0-0 libdbus-1-3 \
  libx11-xcb1 libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-randr0 libxcb-render-util0 libxcb-shape0 \
  libxcb-sync1 libxcb-xfixes0 libxcb-xinerama0 libxcb-xkb1 libxcb-glx0 libxcb-util1 libwayland-client0 libwayland-cursor0 \
  libwayland-egl1 libpulse0 libasound2 libva2 libva-drm2 libva-x11-2 libvdpau1 libxrender1 libxi6 libsm6 libice6 libxext6 \
  libx11-6 libxrandr2 libxss1 libxv1 libxcb-shm0 libdrm2 libxcursor1 libxinerama1 libxdamage1 libxcomposite1 libxtst6 \
  libxfixes3 zlib1g >/dev/null   # the last row: what Qt's FFmpeg media plugin links (x11grab, VA-API)
echo "== toolchain: $(g++-12 --version | head -1), $(cmake --version | head -1), glibc $(ldd --version | head -1 | awk '{print $NF}')"
cmake -S /src -B /tmp/b -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-12 \
  -DCMAKE_PREFIX_PATH=/qt -DCMAKE_DISABLE_FIND_PACKAGE_SDL3=ON -DIONCORE_BUILD_TOOLS=ON >/dev/null
cmake --build /tmp/b 2>&1 | grep -E 'error|warning|Linking' || true
test -x /tmp/b/frontend/ionemu
(cd /tmp/b/ioncore && ctest --output-on-failure 2>&1 | tail -2)
VERSION="$(sed -n 's/.*kVersion = "\([^"]*\)".*/\1/p' /src/frontend/src/app.h)"
APPDIR=/tmp/AppDir; mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/256x256/apps"
cp /tmp/b/frontend/ionemu /tmp/b/ioncore/ionrun /tmp/b/ioncore/ionrepair "$APPDIR/usr/bin/"
cp /src/release/linux/ionemu.desktop "$APPDIR/usr/share/applications/"
cp /src/frontend/branding/icon_256.png "$APPDIR/usr/share/icons/hicolor/256x256/apps/ionemu.png"
cp /src/LICENSE "$APPDIR/LICENSE"; cp /src/frontend/licenses/THIRD_PARTY.txt /src/frontend/licenses/LGPL-3.0.txt "$APPDIR/"
cp /ld/linuxdeploy-x86_64.AppImage /ld/linuxdeploy-plugin-qt-x86_64.AppImage /tmp/; chmod +x /tmp/linuxdeploy-*.AppImage
export APPIMAGE_EXTRACT_AND_RUN=1 QMAKE=/qt/bin/qmake LD_LIBRARY_PATH=/qt/lib VERSION="$VERSION" LINUXDEPLOY_OUTPUT_VERSION="$VERSION"
export EXTRA_QT_PLUGINS="multimedia;tls;iconengines;imageformats;platformthemes;wayland-decoration-client;wayland-graphics-integration-client;wayland-shell-integration"
export EXTRA_PLATFORM_PLUGINS="libqwayland.so"   # run natively on Wayland desktops (xcb is bundled by default; Qt 6.10 has one wayland plugin)
export PATH="/tmp:$PATH"
[ -f /ld/runtime-x86_64 ] && export LDAI_RUNTIME_FILE=/ld/runtime-x86_64   # the AppImage runtime, fetched on the host (github.com/AppImage/type2-runtime)
cd /tmp && linuxdeploy-x86_64.AppImage --appdir "$APPDIR" --plugin qt --output appimage \
  --desktop-file "$APPDIR/usr/share/applications/ionemu.desktop" --icon-file "$APPDIR/usr/share/icons/hicolor/256x256/apps/ionemu.png" 2>&1 | grep -vi 'warning: .*skipping' | tail -15
ls -la /tmp/*.AppImage | grep -v linuxdeploy
cp /tmp/iONemu-*.AppImage /out/ && chown "$OWNER" /out/*.AppImage
echo "== glibc floor of the shipped binaries:"; for b in ionemu ionrun ionrepair; do echo -n "$b: "; objdump -T "$APPDIR/usr/bin/$b" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1; done
INNER
ls -la "$OUT"/*.AppImage
