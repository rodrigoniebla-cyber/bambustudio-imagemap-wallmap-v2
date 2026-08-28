#!/bin/sh
# Apply the OrcaSlicer-ImageMap Phase-1 port to a BambuStudio checkout.
#
# Usage: ./apply.sh /path/to/BambuStudio
#
# The patch was generated against bambulab/BambuStudio commit
# 926a7192574bcb9b3a732e1ec59a46d79cb45466 (version 02.08.02.61).
set -e

TARGET="$1"
if [ -z "$TARGET" ] || [ ! -d "$TARGET/src/libslic3r" ]; then
    echo "usage: $0 /path/to/BambuStudio (a checkout containing src/libslic3r)" >&2
    exit 1
fi
HERE="$(cd "$(dirname "$0")" && pwd)"

# 1. New files (vendored color libraries + the ported module + a unit test).
cp -rv "$HERE/bambustudio/src/." "$TARGET/src/"
cp -rv "$HERE/bambustudio/tests/." "$TARGET/tests/"

# 2. Modifications to existing BambuStudio files.
for PATCH in \
    "$HERE/patches/imagemap-port-modified-files.patch" \
    "$HERE/patches/imagemap-port-gui-wiring.patch" \
    "$HERE/patches/imagemap-port-tests.patch"; do
    if git -C "$TARGET" apply --check "$PATCH" 2>/dev/null; then
        git -C "$TARGET" apply --verbose "$PATCH"
    elif git -C "$TARGET" apply --check -R "$PATCH" 2>/dev/null; then
        echo "$(basename "$PATCH") already applied to $TARGET, skipping."
    else
        echo "$(basename "$PATCH") does not apply cleanly to $TARGET (and isn't already applied)." >&2
        exit 1
    fi
done

echo "Done. Feature toggle: image_map_per_layer_color_rotation (default off)."
echo "GUI: Preferences > Develop mode, then Process Settings > Others > 'Image map per-layer color (experimental)'."
echo "Test: cmake -DSLIC3R_BUILD_TESTS=ON <build dir>, then build+run the 'imagemap_tests' target."
