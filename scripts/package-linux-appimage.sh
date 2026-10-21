#!/usr/bin/env bash
#
# Package the Linux build as an AppImage, mirroring .github/workflows/linux-appimage.yml.
#
#   scripts/package-linux-appimage.sh --linuxdeploy PATH [--build-dir DIR] [--output FILE]
#
# PATH is linuxdeploy-x86_64.AppImage, with linuxdeploy-plugin-qt-x86_64.AppImage in the
# same directory. Build the application first (cmake --build DIR). Stage a copy of
# everything under DIR/AppDir, so the build tree itself is never touched.
#
# The result runs on any system with a glibc and libstdc++ at least as new as the ones this
# was built against: build on the oldest distribution you mean to support.
#
set -euo pipefail

BUILD_DIR=build-release
OUTPUT=""
LINUXDEPLOY=""
TRIPLET="${VCPKG_TARGET_TRIPLET:-x64-linux-release}"

while [ $# -gt 0 ]; do
	case "$1" in
		--build-dir)   BUILD_DIR="$2"; shift 2 ;;
		--output)      OUTPUT="$2"; shift 2 ;;
		--linuxdeploy) LINUXDEPLOY="$2"; shift 2 ;;
		-h|--help)     sed -n '2,12p' "$0"; exit 0 ;;
		*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
done

cd "$(dirname "$0")/.."

[ -x "$LINUXDEPLOY" ] || { echo "--linuxdeploy PATH is required and must be executable" >&2; exit 1; }
[ -x "$BUILD_DIR/MeshLab" ] || { echo "$BUILD_DIR/MeshLab not found - build it first" >&2; exit 1; }
OUTPUT="${OUTPUT:-$BUILD_DIR/MeshLab-x86_64.AppImage}"
case "$OUTPUT" in /*) ;; *) OUTPUT="$PWD/$OUTPUT" ;; esac

# The Qt this build linked against, not whichever is first on PATH: a mismatched one
# deploys the wrong plugins and the result fails to start.
QT_DIR="$(sed -n 's/^Qt6_DIR:[^=]*=//p' "$BUILD_DIR/CMakeCache.txt")"
QT_PREFIX="$(cd "$QT_DIR/../../.." && pwd)"
export QMAKE="$QT_PREFIX/bin/qmake"
[ -x "$QMAKE" ] || { echo "qmake not found under $QT_PREFIX" >&2; exit 1; }

APPDIR="$BUILD_DIR/AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin"
install -m 755 "$BUILD_DIR/MeshLab" "$APPDIR/usr/bin/MeshLab"

# QuadWild's helpers. Beside the executable, which is where the plugin looks, but only
# what the build copied there: that directory is also QuadWild's own build tree.
HELPERS="$BUILD_DIR/quadwild-bimdf"
mkdir -p "$APPDIR/usr/bin/quadwild-bimdf"
cp -a "$HELPERS/bin" "$HELPERS/config" "$HELPERS/LICENSE.txt" "$APPDIR/usr/bin/quadwild-bimdf/"

# The Python standard library. libpython is linked in statically and knows only the prefix
# of the tree that built it, so PythonHost looks for <executable dir>/python instead. Copied
# with -p, and compiled here, because an AppImage is read-only: bytecode caches that are
# missing cannot be written at run time.
PY_EXE="$(sed -n 's/^Python_EXECUTABLE:[^=]*=//p' "$BUILD_DIR/CMakeCache.txt")"
if [ -x "$PY_EXE" ]; then
	PY_STDLIB="$("$PY_EXE" -c 'import sysconfig; print(sysconfig.get_path("stdlib"))')"
	echo "==> bundling the Python standard library ($PY_STDLIB)"
	mkdir -p "$APPDIR/usr/bin/python/lib"
	cp -Rp "$PY_STDLIB" "$APPDIR/usr/bin/python/lib/"
	PY_COPY="$APPDIR/usr/bin/python/lib/$(basename "$PY_STDLIB")"
	rm -rf "$PY_COPY/test" "$PY_COPY/idlelib" "$PY_COPY/turtledemo"
	"$PY_EXE" -m compileall -q "$PY_COPY" || true
else
	echo "==> no Python interpreter in this build; the console will have no standard library"
fi

# Desktop entry and icon. The icon file's name has to match Icon=.
cat > "$BUILD_DIR/MeshLab.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=MeshLab
Comment=View, process and edit 3D meshes
Exec=MeshLab %F
Icon=MeshLab
Categories=Graphics;3DGraphics;Viewer;
Terminal=false
EOF
cp img/MeshLab_Icon_512x512.png "$BUILD_DIR/MeshLab.png"

# linuxdeploy follows the executables' dependencies with ldd, so the libraries the build
# tree reaches through rpath have to be findable now too.
export LD_LIBRARY_PATH="$PWD/$BUILD_DIR/vcpkg_installed/$TRIPLET/lib:$QT_PREFIX/lib:${LD_LIBRARY_PATH:-}"
# offscreen is only for headless smoke tests; xcb is deployed by default.
export EXTRA_PLATFORM_PLUGINS="libqoffscreen.so"
# CI runners have no FUSE, and linuxdeploy and its plugin are AppImages themselves.
export APPIMAGE_EXTRACT_AND_RUN=1
export LDAI_OUTPUT="$OUTPUT"
# linuxdeploy finds its Qt plugin next to itself.
export PATH="$(dirname "$LINUXDEPLOY"):$PATH"

"$LINUXDEPLOY" --appdir "$APPDIR" \
	--executable "$APPDIR/usr/bin/MeshLab" \
	--executable "$APPDIR/usr/bin/quadwild-bimdf/bin/quadwild" \
	--executable "$APPDIR/usr/bin/quadwild-bimdf/bin/quad_from_patches" \
	--desktop-file "$BUILD_DIR/MeshLab.desktop" \
	--icon-file "$BUILD_DIR/MeshLab.png" \
	--plugin qt \
	--output appimage

[ -f "$OUTPUT" ] || { echo "linuxdeploy did not produce $OUTPUT" >&2; exit 1; }
echo "==> $OUTPUT"
