#!/bin/bash
# Build Bambu Studio (arm64) with the OrcaSlicer-ImageMap "one tool-change per
# layer" port applied, and package the result as a DMG.
#
# Runs on a GitHub Actions macOS runner via .github/workflows/main.yml, but
# also works on a real Mac: just run `ci/build-macos.sh` from a checkout of
# this repository.
#
# Env vars (all optional):
#   ARCH              arm64 | x86_64   (default: uname -m)
#   MIN_OSX_VERSION    deployment target (default: 11.0)
#   SKIP_RELEASE       1 to skip publishing a GitHub release (default: 0)
#   GH_TOKEN           used by `gh` to publish a release, if set and not skipped

set -euo pipefail

log() { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }
die() { printf '\n\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }
trap 'die "build failed at line $LINENO (last command: $BASH_COMMAND)"' ERR

if ! REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null)"; then
  REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fi
cd "$REPO_ROOT"

BASE_COMMIT="926a7192574bcb9b3a732e1ec59a46d79cb45466"   # v02.08.02.61, the base the ImageMap patch was made against
UPSTREAM_URL="https://github.com/bambulab/BambuStudio.git"

ARCH="${ARCH:-$(uname -m)}"
[ "$ARCH" = "aarch64" ] && ARCH="arm64"
case "$ARCH" in
  arm64|x86_64) ;;
  *) die "unsupported ARCH '$ARCH' (expected arm64 or x86_64)" ;;
esac
MIN_OSX_VERSION="${MIN_OSX_VERSION:-11.0}"
SKIP_RELEASE="${SKIP_RELEASE:-0}"
NPROC="$(sysctl -n hw.ncpu)"

# Nested under build/ deliberately: macOS's default filesystem (APFS) is
# case-insensitive, so a top-level clone directory named "BambuStudio" would
# collide with this repo's existing "bambustudio/" folder (the vendored port
# sources apply.sh copies from) and cp would refuse to copy onto itself.
BUILD_DIR="$REPO_ROOT/build"
SRC_DIR="$BUILD_DIR/BambuStudio"
DEPS_DIR="$BUILD_DIR/BambuStudio_dep"
DEPS_TARBALL="$REPO_ROOT/BambuStudio_dep_mac_${ARCH}.tar.gz"
INSTALL_DIR="$SRC_DIR/install_dir"
VERSION="02.08.02.61-imagemap"
DMG_NAME="BambuStudio_${ARCH}_${VERSION}_ImageMap.dmg"
DMG_PATH="$REPO_ROOT/$DMG_NAME"

log "Config: ARCH=$ARCH MIN_OSX_VERSION=$MIN_OSX_VERSION NPROC=$NPROC"

# --------------------------------------------------------------------------
log "Installing build prerequisites via Homebrew"
# --------------------------------------------------------------------------
brew install cmake ninja gettext nasm yasm x264 >/dev/null

# --------------------------------------------------------------------------
log "Fetching BambuStudio @ ${BASE_COMMIT:0:12} (v${VERSION%-imagemap})"
# --------------------------------------------------------------------------
if [ ! -d "$SRC_DIR/.git" ]; then
  mkdir -p "$SRC_DIR"
  git -C "$SRC_DIR" init -q
  git -C "$SRC_DIR" remote add origin "$UPSTREAM_URL"
  if ! git -C "$SRC_DIR" fetch --depth 1 origin "$BASE_COMMIT" -q; then
    log "Shallow fetch of pinned commit failed, falling back to full clone"
    git -C "$SRC_DIR" fetch origin -q
  fi
  git -C "$SRC_DIR" checkout -q FETCH_HEAD
fi
[ "$(git -C "$SRC_DIR" rev-parse HEAD)" = "$BASE_COMMIT" ] || die "checked out commit doesn't match pinned base commit $BASE_COMMIT"

# --------------------------------------------------------------------------
log "Applying the ImageMap per-layer-color port"
# --------------------------------------------------------------------------
"$REPO_ROOT/apply.sh" "$SRC_DIR"

# --------------------------------------------------------------------------
log "Aligning deps/CMakeLists.txt's hardcoded macOS deployment target with MIN_OSX_VERSION"
# --------------------------------------------------------------------------
# deps/CMakeLists.txt force-sets CMAKE_OSX_DEPLOYMENT_TARGET to "10.15"
# regardless of what's passed on the cmake command line. Recent SDKs (Xcode
# 16+ / very new Command Line Tools) no longer support libc++ below macOS
# 11.0: the resulting availability #warning gets promoted to a hard error by
# deps that build with -Werror (TBB, Boost's nowide), breaking the deps
# build. Align the forced value with MIN_OSX_VERSION so deps and the app
# agree on the same floor. A no-op if the string was already changed (e.g.
# by upstream) or on a rerun.
sed -i '' \
  "s/set(CMAKE_OSX_DEPLOYMENT_TARGET \"10\.15\" CACHE STRING \"Minimum OS X deployment version\" FORCE)/set(CMAKE_OSX_DEPLOYMENT_TARGET \"$MIN_OSX_VERSION\" CACHE STRING \"Minimum OS X deployment version\" FORCE)/" \
  "$SRC_DIR/deps/CMakeLists.txt"

# --------------------------------------------------------------------------
if [ -f "$DEPS_TARBALL" ]; then
  log "Restoring cached dependency build from $(basename "$DEPS_TARBALL")"
  mkdir -p "$DEPS_DIR"
  tar -xzf "$DEPS_TARBALL" -C "$DEPS_DIR"
else
  log "No dependency cache found - building deps from source (this is the slow part, ~1-2h)"
  mkdir -p "$SRC_DIR/deps/build"
  (
    cd "$SRC_DIR/deps/build"
    cmake .. \
      -DDESTDIR="$DEPS_DIR" \
      -DOPENSSL_ARCH="darwin64-${ARCH}-cc" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_OSX_VERSION"
    cmake --build . --config Release -j"$NPROC"
  )
  log "Caching dependency build to $(basename "$DEPS_TARBALL")"
  tar -czf "$DEPS_TARBALL" -C "$DEPS_DIR" .
fi

# --------------------------------------------------------------------------
log "Configuring BambuStudio"
# --------------------------------------------------------------------------
mkdir -p "$SRC_DIR/build"
(
  cd "$SRC_DIR/build"
  # SLIC3R_BUILD_TESTS is pinned OFF rather than left to its default: this
  # script reuses $SRC_DIR and its CMake cache across runs, so a developer who
  # had previously configured that tree with -DSLIC3R_BUILD_TESTS=ON (e.g. to
  # run imagemap_tests) would otherwise carry the cached ON into this build,
  # and `--target install` would then try to build the upstream test suites.
  # Several of those no longer compile against 02.08.02.61 (libnest2d_tests,
  # libslic3r_tests and fff_print_tests all reference APIs that have since
  # changed), which fails the DMG build for reasons entirely unrelated to it.
  cmake .. \
    -DBBL_RELEASE_TO_PUBLIC=1 \
    -DSLIC3R_BUILD_TESTS=OFF \
    -DCMAKE_PREFIX_PATH="$DEPS_DIR/usr/local" \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_OSX_VERSION" \
    -DCMAKE_MACOSX_RPATH=ON \
    -DCMAKE_MACOSX_BUNDLE=on \
    -G Ninja

  log "Building BambuStudio (this is the other slow part, ~30-60min)"
  cmake --build . --target install --config Release -j"$NPROC"
)

APP_BUNDLE="$(find "$INSTALL_DIR" -maxdepth 2 -name '*.app' -print -quit)"
[ -n "$APP_BUNDLE" ] && [ -d "$APP_BUNDLE" ] || die "no .app bundle found under $INSTALL_DIR after install"
log "Built app bundle: $APP_BUNDLE"

# --------------------------------------------------------------------------
log "Making the app bundle relocatable (rewriting any absolute build-machine"
log "library paths so it doesn't crash with a dyld error on another Mac)"
# --------------------------------------------------------------------------
# Most of the app is built with prefixes under $BUILD_DIR (the deps DESTDIR
# and the BambuStudio checkout itself), but some of the from-source deps'
# CMake configure steps find and link against a Homebrew-provided library
# already present on the runner instead of building their own (this is how a
# real crash happened: BambuStudio.app ended up depending on this CI runner's
# /opt/homebrew/*/libzstd.1.dylib, which obviously isn't on anyone else's
# Mac). Anything the app links against under $BUILD_DIR, /opt/homebrew, or
# /usr/local (Homebrew's Apple Silicon and Intel prefixes) only exists on
# this CI runner - copy those into Contents/Frameworks, rewrite the
# references to @rpath, and re-sign, so the app is fully self-contained.
fixup_app_bundle() {
  local app="$1"
  local frameworks="$app/Contents/Frameworks"
  mkdir -p "$frameworks"

  local exe
  exe="$(find "$app/Contents/MacOS" -maxdepth 1 -type f -perm -111 -print -quit)"
  [ -n "$exe" ] || die "no executable found under $app/Contents/MacOS"

  # macOS's system bash is the ancient 3.2 (last GPLv2 release), which has a
  # known bug: expanding a zero-element array with "${arr[@]}" under `set -u`
  # throws "unbound variable" instead of expanding to nothing. Track "seen"
  # as a delimited string instead of an array to sidestep it entirely.
  local -a queue=("$exe")
  local seen=$'\n'
  local bin dep libname dest

  while [ "${#queue[@]}" -gt 0 ]; do
    bin="${queue[0]}"
    queue=("${queue[@]:1}")

    case "$seen" in *$'\n'"$bin"$'\n'*) continue ;; esac
    seen="$seen$bin"$'\n'
    [ -f "$bin" ] || continue
    chmod u+w "$bin" 2>/dev/null || true

    while IFS= read -r dep; do
      [ -n "$dep" ] || continue
      case "$dep" in
        "$BUILD_DIR"/*|/opt/homebrew/*|/usr/local/*)
          libname="$(basename "$dep")"
          dest="$frameworks/$libname"
          if [ ! -f "$dest" ]; then
            cp -L "$dep" "$dest"
            chmod u+w "$dest"
            queue+=("$dest")
          fi
          install_name_tool -change "$dep" "@rpath/$libname" "$bin"
          ;;
      esac
    done < <(otool -L "$bin" | tail -n +2 | awk '{print $1}')

    case "$bin" in
      "$frameworks"/*) install_name_tool -id "@rpath/$(basename "$bin")" "$bin" 2>/dev/null || true ;;
    esac
  done

  otool -l "$exe" | grep -q "@executable_path/../Frameworks" \
    || install_name_tool -add_rpath "@executable_path/../Frameworks" "$exe"

  log "Ad-hoc code-signing $app (unnotarized - Gatekeeper will still require right-click Open on first launch)"
  codesign --force --deep --sign - "$app"
}
fixup_app_bundle "$APP_BUNDLE"

# --------------------------------------------------------------------------
log "Packaging $DMG_NAME"
# --------------------------------------------------------------------------
STAGE_DIR="$(mktemp -d)"
trap 'rm -rf "$STAGE_DIR"' EXIT
cp -R "$APP_BUNDLE" "$STAGE_DIR/"
ln -s /Applications "$STAGE_DIR/Applications"
rm -f "$DMG_PATH"
hdiutil create -volname "BambuStudio ImageMap" -srcfolder "$STAGE_DIR" -ov -format UDZO "$DMG_PATH"
log "DMG ready: $DMG_PATH ($(du -h "$DMG_PATH" | cut -f1))"

# --------------------------------------------------------------------------
if [ "$SKIP_RELEASE" != "1" ] && [ -n "${GH_TOKEN:-}" ] && command -v gh >/dev/null; then
  log "Publishing GitHub release"
  TAG="imagemap-${VERSION}-$(date +%Y%m%d%H%M)"
  if gh release create "$TAG" "$DMG_PATH" \
      --title "BambuStudio ImageMap port ($VERSION)" \
      --notes "Automated macOS build. Base: bambulab/BambuStudio@${BASE_COMMIT:0:12}. Feature toggle image_map_per_layer_color_rotation is off by default." \
      --repo "${GITHUB_REPOSITORY:-}"; then
    log "Release published: tag $TAG"
  else
    log "WARNING: release publish failed - the DMG is still available as a workflow artifact"
  fi
else
  log "Skipping release publish (SKIP_RELEASE=$SKIP_RELEASE, GH_TOKEN set: $([ -n "${GH_TOKEN:-}" ] && echo yes || echo no))"
fi

log "Done."
