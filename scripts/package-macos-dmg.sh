#!/usr/bin/env bash
#
# Build a MeshLab .dmg locally, mirroring .github/workflows/macos-dmg.yml.
#
#   scripts/package-macos-dmg.sh [--build-dir DIR] [--no-build] [--jobs N]
#       [--sign-identity "Developer ID Application: ..."]
#
# Unlike the CI job this stages a copy of the bundle, so the build tree is never
# mutated by macdeployqt and repeated runs stay reproducible.
#
set -euo pipefail

APP_NAME=MeshLab
BUILD_DIR=build-release
DO_BUILD=1
JOBS=""
SIGN_IDENTITY="${MACOS_SIGNING_IDENTITY:-}"

while [ $# -gt 0 ]; do
	case "$1" in
		--build-dir) BUILD_DIR="$2"; shift 2 ;;
		--no-build)  DO_BUILD=0; shift ;;
		--jobs)      JOBS="$2"; shift 2 ;;
		--sign-identity) SIGN_IDENTITY="$2"; shift 2 ;;
		-h|--help)   sed -n '2,10p' "$0"; exit 0 ;;
		*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
done

cd "$(dirname "$0")/.."

[ -f "$BUILD_DIR/CMakeCache.txt" ] || {
	echo "No CMake cache in $BUILD_DIR - configure it first, e.g." >&2
	echo "  cmake --preset vcpkg-manifest -B $BUILD_DIR -DCMAKE_BUILD_TYPE=Release" >&2
	exit 1
}

if [ "$DO_BUILD" -eq 1 ]; then
	cmake --build "$BUILD_DIR" ${JOBS:+-j "$JOBS"}
fi

STAGE="$BUILD_DIR/dist"
APP="$STAGE/$APP_NAME.app"

# Prune the staging dir, or a previous run's copy is picked as the source and
# then destroyed by the rm below.
APP_SRC="$(find "$BUILD_DIR" -path "$STAGE" -prune -o \
	-maxdepth 3 -type d -name "$APP_NAME.app" -print -quit)"
[ -n "$APP_SRC" ] || { echo "Could not find $APP_NAME.app under $BUILD_DIR" >&2; exit 1; }

# Use the macdeployqt belonging to the Qt this build actually linked against;
# a mismatched one from PATH silently produces a bundle that will not launch.
QT_DIR="$(sed -n 's/^Qt6_DIR:[^=]*=//p' "$BUILD_DIR/CMakeCache.txt")"
MACDEPLOYQT="$(cd "$QT_DIR/../../.." && pwd)/bin/macdeployqt"
[ -x "$MACDEPLOYQT" ] || MACDEPLOYQT="$(command -v macdeployqt || true)"
[ -x "$MACDEPLOYQT" ] || { echo "macdeployqt not found (Qt6_DIR=$QT_DIR)" >&2; exit 1; }

rm -rf "$STAGE"
mkdir -p "$STAGE"
cp -R "$APP_SRC" "$APP"

# Contents/MacOS holds executables only. Anything else is leftover output from
# running the app in place, and would otherwise be shipped inside the dmg.
find "$APP/Contents/MacOS" -type f | while IFS= read -r f; do
	file "$f" | grep -q "Mach-O" || { echo "==> dropping stray $(basename "$f")"; rm -f "$f"; }
done

echo "==> macdeployqt ($MACDEPLOYQT)"
# Homebrew splits Qt across formulas (qtbase, qtsvg, ...), so modules outside
# qtbase are not on the main binary's rpath; point macdeployqt at the umbrella
# lib dir so it can resolve them.
QT_LIBDIR="$(cd "$QT_DIR/../.." && pwd)"
"$MACDEPLOYQT" "$APP" -always-overwrite -libpath="$QT_LIBDIR" -no-codesign

# Bundle libomp: it is a Homebrew dylib outside the .app, so an absolute link
# path would break on any machine that does not have it installed.
APP_BIN="$APP/Contents/MacOS/$APP_NAME"
OMP_LINK_PATH="$(otool -L "$APP_BIN" | awk '/libomp\.dylib/ {print $1; exit}')"
if [ -n "$OMP_LINK_PATH" ]; then
	echo "==> embedding libomp"
	OMP_ROOT="${OpenMP_ROOT:-$(brew --prefix libomp)}"
	mkdir -p "$APP/Contents/Frameworks"
	cp -f "$OMP_ROOT/lib/libomp.dylib" "$APP/Contents/Frameworks/"
	chmod u+w "$APP/Contents/Frameworks/libomp.dylib"
	install_name_tool -id "@rpath/libomp.dylib" "$APP/Contents/Frameworks/libomp.dylib"
	install_name_tool -change "$OMP_LINK_PATH" \
		"@executable_path/../Frameworks/libomp.dylib" "$APP_BIN"
fi

# Ship the Python standard library. libpython is linked in statically and knows
# only the prefix of the tree that built it, so PythonHost points the interpreter
# at Contents/Resources/python instead — which leaves the console without a
# standard library unless the stdlib is actually put there. Copied with -p
# because the .pyc caches beside each module are only used while the source
# timestamps they record still match, and a read-only bundle cannot rebuild them.
PY_EXE="$(sed -n 's/^Python_EXECUTABLE:[^=]*=//p' "$BUILD_DIR/CMakeCache.txt")"
if [ -x "$PY_EXE" ]; then
	PY_STDLIB="$("$PY_EXE" -c 'import sysconfig; print(sysconfig.get_path("stdlib"))')"
	echo "==> bundling the Python standard library ($PY_STDLIB)"
	mkdir -p "$APP/Contents/Resources/python/lib"
	cp -Rp "$PY_STDLIB" "$APP/Contents/Resources/python/lib/"
else
	echo "==> no Python interpreter in this build; the console will have no standard library"
fi

# The bundled Python standard library contains native extension modules (.so) in
# lib-dynload. macdeployqt does not discover and sign these binaries, so notarization
# rejects the app unless they are signed before the enclosing app is sealed.
if [ -n "$SIGN_IDENTITY" ] && [ -d "$APP/Contents/Resources/python" ]; then
	echo "==> signing bundled Python Mach-O binaries"
	find "$APP/Contents/Resources/python" -type f -print0 | while IFS= read -r -d '' pybin; do
		if file "$pybin" | grep -q 'Mach-O'; then
			chmod u+w "$pybin"
			codesign --force --options runtime --timestamp \
				--sign "$SIGN_IDENTITY" "$pybin"
			codesign --verify --strict --verbose=2 "$pybin"
		fi
	done
fi

# macdeployqt signs the main executable, frameworks, and Qt plug-ins, but it
# does not discover project-specific Mach-O executables stored in Helpers.
# Sign those nested components before macdeployqt seals the enclosing app.
if [ -n "$SIGN_IDENTITY" ] && [ -d "$APP/Contents/Helpers" ]; then
	echo "==> signing custom helper executables"
	find "$APP/Contents/Helpers" -type f -print0 | while IFS= read -r -d '' helper; do
		if file "$helper" | grep -q 'Mach-O'; then
			echo "    $(basename "$helper")"
			chmod u+w "$helper"
			codesign --force --options runtime --timestamp \
				--sign "$SIGN_IDENTITY" "$helper"
			codesign --verify --strict --verbose=2 "$helper"
		fi
	done
fi

echo "==> building dmg"
if [ -n "$SIGN_IDENTITY" ]; then
	echo "==> Developer ID signing with hardened runtime"
	"$MACDEPLOYQT" "$APP" -always-overwrite -libpath="$QT_LIBDIR" \
		"-sign-for-notarization=$SIGN_IDENTITY" -dmg
else
	echo "==> ad-hoc signing"
	"$MACDEPLOYQT" "$APP" -always-overwrite -libpath="$QT_LIBDIR" \
		-codesign=- -dmg
fi
codesign --verify --deep --strict --verbose=2 "$APP"
GENERATED_DMG="$STAGE/$APP_NAME.dmg"
[ -f "$GENERATED_DMG" ] || { echo "macdeployqt did not produce $GENERATED_DMG" >&2; exit 1; }

# Give the mounted volume the app icon instead of the generic disk image one.
APP_ICON="$(find "$APP/Contents/Resources" -maxdepth 1 -type f -name '*.icns' -print -quit)"
if [ -n "$APP_ICON" ] && xcrun -f SetFile >/dev/null 2>&1; then
	echo "==> setting volume icon"
	RW_DMG="$STAGE/$APP_NAME-rw.dmg"
	# hdiutil reports the resolved path, so /tmp would never match the device line.
	MOUNT_POINT="$(cd "$(mktemp -d "/tmp/$APP_NAME.XXXXXX")" && pwd -P)"
	hdiutil convert "$GENERATED_DMG" -format UDRW -o "${RW_DMG%.dmg}" >/dev/null
	DEVICE_NAME="$(hdiutil attach "$RW_DMG" -mountpoint "$MOUNT_POINT" -nobrowse -readwrite |
		awk -v mp="$MOUNT_POINT" '$NF == mp {print $1; exit}')"
	if [ -n "$DEVICE_NAME" ]; then
		cp -f "$APP_ICON" "$MOUNT_POINT/.VolumeIcon.icns"
		xcrun SetFile -a C "$MOUNT_POINT"
		xcrun SetFile -a V "$MOUNT_POINT/.VolumeIcon.icns"
		sync
		hdiutil detach "$DEVICE_NAME" >/dev/null
		rm -f "$GENERATED_DMG"
		hdiutil convert "$RW_DMG" -format UDZO -imagekey zlib-level=9 \
			-o "${GENERATED_DMG%.dmg}" >/dev/null
		rm -f "$RW_DMG"
	else
		echo "Could not mount dmg; leaving default volume icon" >&2
		hdiutil detach "$MOUNT_POINT" 2>/dev/null || true
	fi
	rmdir "$MOUNT_POINT" 2>/dev/null || true
fi

if [ -n "$SIGN_IDENTITY" ]; then
	DMG="$BUILD_DIR/$APP_NAME-macos-$(uname -m).dmg"
else
	DMG="$BUILD_DIR/$APP_NAME-macos-$(uname -m)-unsigned.dmg"
fi
rm -f "$DMG"
mv "$GENERATED_DMG" "$DMG"
if [ -n "$SIGN_IDENTITY" ]; then
	echo "==> signing dmg"
	codesign --force --timestamp --sign "$SIGN_IDENTITY" "$DMG"
	codesign --verify --verbose=2 "$DMG"
fi
echo
echo "==> $DMG"
ls -lah "$DMG"
