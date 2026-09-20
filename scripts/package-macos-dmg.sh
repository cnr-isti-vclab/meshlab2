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

# Bundle geogram. It is the only dynamically linked vcpkg dependency: its port
# forces dynamic linkage on Darwin, so unlike the other 43 it cannot be absorbed
# into the binary and macdeployqt, which only chases Qt's own libraries, leaves
# it behind.
#
# BOTH dylibs are needed, even though only the first is linked. geogram's
# spectral methods -- spectral LSCM, the spectral chart parametrizer, the
# manifold-harmonic segmenters -- ask OpenNL for its ARPACK extension, which
# dlopen()s "libarpack.dylib", fails, and retries with
# "libgeogram_num_3rdparty.dylib": geogram's vendored ARPACK, which the port
# installs as a separate library that nothing links against. Drop it and those
# filters do not fail loudly, they refuse -- so it has to be here deliberately
# rather than by accident. The plain-name dlopen resolves through the main
# binary's LC_RPATH, which macdeployqt already points at Contents/Frameworks.
VCPKG_LIB="$BUILD_DIR/vcpkg_installed/arm64-osx/lib"
if [ -d "$VCPKG_LIB" ]; then
	mkdir -p "$APP/Contents/Frameworks"
	for geolib in libgeogram libgeogram_num_3rdparty; do
		# Resolve the versioned real file behind the unversioned symlink, and
		# copy it under the SONAME the linker recorded, not the symlink name.
		src="$(cd "$VCPKG_LIB" && python3 -c "import os,sys; print(os.path.realpath(sys.argv[1]))" "$geolib.dylib" 2>/dev/null || true)"
		[ -n "$src" ] && [ -f "$src" ] || { echo "==> $geolib.dylib not found, skipping" >&2; continue; }
		soname="$(basename "$src")"
		echo "==> embedding $soname"
		cp -f "$src" "$APP/Contents/Frameworks/$soname"
		chmod u+w "$APP/Contents/Frameworks/$soname"
		install_name_tool -id "@rpath/$soname" "$APP/Contents/Frameworks/$soname"
		# The ARPACK fallback dlopen()s the unversioned leaf name, so it has to
		# exist beside the versioned file.
		ln -sf "$soname" "$APP/Contents/Frameworks/$geolib.dylib"
		# Repoint the main binary from the build tree's absolute path.
		old_path="$(otool -L "$APP_BIN" | awk -v n="$soname" '$1 ~ n {print $1; exit}')"
		[ -n "$old_path" ] && install_name_tool -change "$old_path" \
			"@executable_path/../Frameworks/$soname" "$APP_BIN"
	done
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

# Strip build-tree rpaths. Three sources leave absolute LC_RPATH entries pointing
# into somebody's build directory, none of which can resolve on a user's machine:
# vcpkg's toolchain adds its debug lib dir to every target we link; macdeployqt
# only rewrites the rpaths it recognises; and the bundled Python extension modules
# were built elsewhere entirely -- theirs still name QMeshLab/build-debug, a
# directory that stopped existing at the 2026-09 rename.
#
# They are dead weight rather than a fault: with geogram repointed at
# @executable_path above, the main binary has no absolute dylib references left at
# all. But they ship the packager's home directory inside the binary, and a stale
# rpath that happens to exist on a developer's machine is exactly how a missing
# bundled library goes unnoticed until someone else runs the app.
#
# Only build-tree paths are removed, matched on /vcpkg_installed/. Homebrew Cellar
# rpaths on third-party dylibs macdeployqt copied are left alone: those libraries
# resolve their own dependencies through them, and rewriting other people's
# libraries is macdeployqt's job, not ours. Must run before the signing loops
# below, since install_name_tool invalidates a signature.
echo "==> stripping build-tree rpaths"
STRIPPED_RPATHS=0
while IFS= read -r -d '' macho; do
	file "$macho" | grep -q 'Mach-O' || continue
	# Collected into a variable rather than piped into the loop: this script runs
	# under `set -o pipefail`, and a grep that matches nothing -- which is most
	# files -- would fail the pipeline and abort the packaging run.
	stale_list="$(
		otool -l "$macho" 2>/dev/null \
			| awk '/LC_RPATH/{getline; getline; sub(/^ *path /,""); sub(/ \(offset.*/,""); print}' \
			| grep -v '^@' | grep '/vcpkg_installed/' || true
	)"
	[ -n "$stale_list" ] || continue
	while IFS= read -r stale; do
		[ -n "$stale" ] || continue
		chmod u+w "$macho"
		install_name_tool -delete_rpath "$stale" "$macho" 2>/dev/null || true
		STRIPPED_RPATHS=$((STRIPPED_RPATHS + 1))
	done <<< "$stale_list"
done < <(find "$APP" -type f -print0)
echo "    removed $STRIPPED_RPATHS build-tree rpath entries"

# The bundled Python standard library contains native extension modules (.so) in
# lib-dynload. macdeployqt does not discover and sign these binaries, so notarization
# rejects the app unless they are signed before the enclosing app is sealed.
if [ -n "$SIGN_IDENTITY" ] && [ -d "$APP/Contents/Resources/python" ]; then
	echo "==> signing bundled Python Mach-O binaries"
	while IFS= read -r -d '' pybin; do
		if file "$pybin" | grep -q 'Mach-O'; then
			chmod u+w "$pybin"
			codesign --force --options runtime --timestamp \
				--sign "$SIGN_IDENTITY" "$pybin"
			codesign --verify --strict --verbose=2 "$pybin"
		fi
	done < <(find "$APP/Contents/Resources/python" -type f -print0)
fi

# macdeployqt signs the frameworks it placed itself; the geogram dylibs above were
# copied in by hand after it ran, so they carry no signature and notarization
# rejects the app. Same pattern as the two loops around this one.
if [ -n "$SIGN_IDENTITY" ] && [ -d "$APP/Contents/Frameworks" ]; then
	echo "==> signing hand-bundled dylibs"
	for dylib in "$APP/Contents/Frameworks"/libgeogram*.dylib "$APP/Contents/Frameworks/libomp.dylib"; do
		[ -f "$dylib" ] && [ ! -L "$dylib" ] || continue
		echo "    $(basename "$dylib")"
		chmod u+w "$dylib"
		codesign --force --options runtime --timestamp \
			--sign "$SIGN_IDENTITY" "$dylib"
		codesign --verify --strict --verbose=2 "$dylib"
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
