#!/usr/bin/env bash
#
# Compare the vendored Open3D subset against an upstream checkout.
#
#   plugins/filter_open3d/check_upstream_status.sh [path-to-open3d]
#
# Default path is ../../.reference/open3d, matching the convention the
# Instant Meshes plugin uses. See UPSTREAM.md for the vendored revision.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
UPSTREAM_SRC="${1:-$HERE/../../.reference/open3d}/cpp"

[ -d "$UPSTREAM_SRC" ] || {
	echo "No Open3D sources at $UPSTREAM_SRC" >&2
	echo "Clone one, or pass the path: $0 /path/to/Open3D" >&2
	exit 1
}

differ=0 missing=0 same=0
while IFS= read -r f; do
	[ -n "$f" ] || continue
	# The 3rdparty headers live outside cpp/ upstream.
	case "$f" in
		3rdparty/*) up="$UPSTREAM_SRC/../3rdparty/${f#3rdparty/}"
		            up="${up/tomasakeninemoeller\//tomasakeninemoeller/include/tomasakeninemoeller/}" ;;
		*)          up="$UPSTREAM_SRC/$f" ;;
	esac
	if [ ! -f "$up" ]; then
		echo "  MISSING upstream: $f"
		missing=$((missing + 1))
	elif ! cmp -s "$HERE/upstream/$f" "$up"; then
		echo "  PATCHED locally : $f"
		differ=$((differ + 1))
	else
		same=$((same + 1))
	fi
done < "$HERE/files.txt"

echo
echo "  identical: $same   patched: $differ   missing upstream: $missing"
echo
echo "Expected: exactly one patched file, open3d/geometry/BoundingVolume.cpp."
echo "Anything else is either an undocumented local change or upstream drift."

# Files present in the tree but absent from the manifest.
extra="$(cd "$HERE/upstream" && find . \( -name '*.h' -o -name '*.cpp' \) | sed 's|^\./||' | sort | comm -13 "$HERE/files.txt" -)"
if [ -n "$extra" ]; then
	echo
	echo "Vendored but not listed in files.txt:"
	echo "$extra" | sed 's/^/  /'
fi
