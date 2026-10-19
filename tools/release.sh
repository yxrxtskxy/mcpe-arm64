#!/bin/sh
# One command from a fresh checkout to a release (Git Bash on Windows):
#   sh tools/release.sh [--install]
# Sets up whatever is missing, builds the release APK and packs it in
# out/releases/<versionName>-arm64.<versionCode>/ with its checksum and a record of what went into it. Finished steps are skipped, so re-running
# only rebuilds. --install then puts it on the connected phone with `adb install -r`, which keeps
# the worlds (ANDROID_SERIAL picks one of several phones).
#
# A new release needs a new versionCode: bump it in android/app/build.gradle.kts. versionName is
# the game APK's own (1.1.5.1). Re-running with the same versionCode replaces that release.
#
# Where each piece comes from (each is skipped once present):
#   Android SDK, NDK r29, JDK 17, Gradle   tools/setup_toolchain_windows.sh, checksum-verified
#   upstream/ZettaBridge                   tools/fetch_zettabridge.sh: pinned commits + patches/
#   arm32 sysroot                          upstream tools/extract_sysroot.sh, in WSL (debugfs)
#   Boost headers                          WSL's libboost-dev, copied into .deps/boost-headers
#   signing key                            tools/make_signing_key.sh (~/.mcpe-arm64; back it up)
# Needs on PATH: git, curl, unzip, cmake, ninja, python3. WSL only while the sysroot or the Boost
# headers are missing.
#
# For quick test builds while changing the runtime, use tools/build_apk.sh: this script refuses to
# build from a ZettaBridge checkout that patches/ does not reproduce.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ZB="$ROOT/upstream/ZettaBridge"
SDK=${ANDROID_SDK_ROOT:-$(cygpath -u "$LOCALAPPDATA")/Android/Sdk}
BUILD_TOOLS="$SDK/build-tools/35.0.0"
NDK_DIR="$SDK/ndk/29.0.14206865"
BOOST="$ROOT/.deps/boost-headers"
SIGNING="$HOME/.mcpe-arm64/signing.properties"
export GAME_APK=${GAME_APK:-$ROOT/game/minecraft-1.1.5.apk}
export JAVA_HOME=${MCPE_JAVA_HOME:-$(cygpath -m "$HOME/.jdks/temurin-17")}
WSL_DISTRO=${WSL_DISTRO:-Ubuntu-24.04}

INSTALL=0
case "${1:-}" in
    "") ;;
    --install) INSTALL=1 ;;
    *) echo "usage: sh tools/release.sh [--install]" >&2; exit 2 ;;
esac

step() { printf '\n######## %s\n' "$*"; }
die() { printf 'release: %s\n' "$*" >&2; exit 1; }
# Runs a shell snippet in WSL; paths go in as Windows paths (cygpath -m) and wslpath converts them.
in_wsl() {
    command -v wsl.exe >/dev/null || die "WSL is needed for this step (with $WSL_DISTRO installed)"
    snippet=$1
    shift
    MSYS_NO_PATHCONV=1 wsl.exe -d "$WSL_DISTRO" --exec sh -c "$snippet" sh "$@"
}

SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT
INDEX="$(cygpath -m "$SCRATCH")/index"
# The tree a fresh tools/fetch_zettabridge.sh would check out: HEAD plus the patches, in order.
patched_tree() {  # repo patch...
    repo=$1
    shift
    rm -f "$INDEX"
    GIT_INDEX_FILE="$INDEX" git -C "$repo" read-tree HEAD
    for patch in "$@"; do
        GIT_INDEX_FILE="$INDEX" git -C "$repo" apply --cached --whitespace=nowarn "$patch"
    done
    GIT_INDEX_FILE="$INDEX" git -C "$repo" write-tree
}
# The tree actually checked out: tracked files (git add -N ones included) as they are on disk.
worktree_tree() {  # repo
    cp "$(git -C "$1" rev-parse --absolute-git-dir)/index" "$INDEX"
    GIT_INDEX_FILE="$INDEX" git -C "$1" add -u
    GIT_INDEX_FILE="$INDEX" git -C "$1" write-tree
}
pin() { sed -n "s/^$1=//p" "$ROOT/tools/fetch_zettabridge.sh"; }

step "host tools and inputs"
for tool in git curl unzip sha256sum cmake ninja python3; do
    command -v "$tool" >/dev/null || die "$tool is not on PATH"
done
[ -f "$GAME_APK" ] || die "missing game APK: $GAME_APK (copy minecraft-1.1.5.apk into game/)"
echo "game APK: $GAME_APK"

step "toolchain"
sh "$ROOT/tools/setup_toolchain_windows.sh"

step "ZettaBridge sources"
if [ ! -d "$ZB" ]; then
    sh "$ROOT/tools/fetch_zettabridge.sh"
fi
ZETTA_COMMIT=$(pin ZETTA_COMMIT)
DYNARMIC_COMMIT=$(pin DYNARMIC_COMMIT)
[ "$(git -C "$ZB" rev-parse HEAD)" = "$ZETTA_COMMIT" ] ||
    die "upstream/ZettaBridge is not on the pinned commit $ZETTA_COMMIT"
[ "$(git -C "$ZB/third_party/dynarmic" rev-parse HEAD)" = "$DYNARMIC_COMMIT" ] ||
    die "third_party/dynarmic is not on the pinned commit $DYNARMIC_COMMIT"
# What gets built must be what patches/ records, or the release cannot be rebuilt later.
expected=$(patched_tree "$ZB/third_party/dynarmic" "$ZB"/third_party/patches/dynarmic-*.patch \
    "$ROOT"/patches/dynarmic/*.patch)
actual=$(worktree_tree "$ZB/third_party/dynarmic")
if [ "$expected" != "$actual" ]; then
    git -C "$ZB/third_party/dynarmic" diff-tree -r --stat "$expected" "$actual" >&2
    die "third_party/dynarmic differs from ZettaBridge's Dynarmic patches plus patches/dynarmic
(above: patches -> checkout). Save this project's part as patches/dynarmic/*.patch: the diff of the
checkout against the pinned commit with ZettaBridge's third_party/patches applied."
fi
expected=$(patched_tree "$ZB" "$ROOT"/patches/zettabridge/*.patch)
actual=$(worktree_tree "$ZB")
if [ "$expected" != "$actual" ]; then
    git -C "$ZB" diff-tree -r --stat "$expected" "$actual" >&2
    die "upstream/ZettaBridge differs from patches/zettabridge (above: patch -> checkout). Save the
checkout into the patch (git add -N any new file first), then run this again:
  cd upstream/ZettaBridge && git diff --binary -- . ':(exclude)third_party/dynarmic' > ../../patches/zettabridge/0001-mcpe-arm64.patch"
fi
untracked=$(git -C "$ZB" ls-files --others --exclude-standard)
if [ -n "$untracked" ]; then
    echo "note: untracked, so not in patches/ (git add -N to include):"
    printf '%s\n' "$untracked" | sed 's/^/  /'
fi
echo "ZettaBridge $ZETTA_COMMIT + patches/zettabridge: matches the checkout"

step "arm32 sysroot"
if [ -s "$ZB/sysroot/system/bin/linker" ] && [ -s "$ZB/sysroot/system/lib/libstdc++.so" ]; then
    echo "present"
else
    in_wsl 'command -v debugfs >/dev/null || {
            echo "WSL needs e2fsprogs: wsl -d $2 -u root apt-get install -y e2fsprogs curl unzip" >&2
            exit 1
        }
        cd "$(wslpath -u "$1")" && sh tools/extract_sysroot.sh' "$(cygpath -m "$ZB")" "$WSL_DISTRO"
fi

step "Boost headers"
if [ -f "$BOOST/boost/version.hpp" ]; then
    echo "present"
else
    in_wsl '[ -f /usr/include/boost/version.hpp ] || {
            echo "WSL needs libboost-dev: wsl -d $2 -u root apt-get install -y libboost-dev" >&2
            exit 1
        }
        dest=$(wslpath -u "$1")
        rm -rf "$dest.part" && mkdir -p "$dest.part"
        tar -C /usr/include -cf - boost | tar -C "$dest.part" -xf - && mv "$dest.part" "$dest"' \
        "$(cygpath -m "$BOOST")" "$WSL_DISTRO"
fi

step "signing key"
NEW_KEY=0
if [ -f "$SIGNING" ]; then
    echo "~/.mcpe-arm64 (existing)"
else
    sh "$ROOT/tools/make_signing_key.sh"
    NEW_KEY=1
fi

step "build"
BUILD_TYPE=release sh "$ROOT/tools/build_apk.sh"

step "release"
APK="$ROOT/out/MinecraftPE-1.1.5-arm64.apk"
BADGING=$("$BUILD_TOOLS/aapt2.exe" dump badging "$(cygpath -m "$APK")" | tr -d '\r')
if printf '%s\n' "$BADGING" | grep -q '^application-debuggable'; then die "the built APK is debuggable"; fi
PACKAGE_LINE=$(printf '%s\n' "$BADGING" | grep '^package:')
PACKAGE=$(printf '%s\n' "$PACKAGE_LINE" | sed "s/.* name='\([^']*\)'.*/\1/")
VERSION_CODE=$(printf '%s\n' "$PACKAGE_LINE" | sed "s/.* versionCode='\([^']*\)'.*/\1/")
VERSION_NAME=$(printf '%s\n' "$PACKAGE_LINE" | sed "s/.* versionName='\([^']*\)'.*/\1/")
# versionName is the game's, the same for every release, so the release is named by versionCode too.
RELEASE="$VERSION_NAME-arm64.$VERSION_CODE"
CERT=$("$BUILD_TOOLS/apksigner.bat" verify --print-certs "$(cygpath -m "$APK")" 2>/dev/null | tr -d '\r' |
    sed -n 's/^Signer #1 certificate SHA-256 digest: //p')
[ -n "$CERT" ] || die "apksigner could not verify $APK"
GAME_INFO=$(unzip -p "$APK" assets/game/info.properties | tr -d '\r')
NDK_VERSION=$(sed -n 's/^Pkg.Revision *= *//p' "$NDK_DIR/source.properties" | tr -d '\r')
BOOST_VERSION=$(sed -n 's/^#define BOOST_LIB_VERSION "\(.*\)"/\1/p' "$BOOST/boost/version.hpp")

DEST="$ROOT/out/releases/$RELEASE"
NAME="MinecraftPE-$RELEASE.apk"
if [ -d "$DEST" ]; then
    echo "replacing release $RELEASE (for a new one, bump versionCode in android/app/build.gradle.kts)"
fi
rm -rf "$DEST"
mkdir -p "$DEST"
cp "$APK" "$DEST/$NAME"
(cd "$DEST" && sha256sum "$NAME" > SHA256SUMS)
{
    echo "Minecraft PE 1.1.5 for arm64-only phones, release $RELEASE"
    echo
    echo "apk            $NAME"
    echo "sha256         $(cut -d' ' -f1 "$DEST/SHA256SUMS")"
    echo "package        $PACKAGE, versionCode $VERSION_CODE, versionName $VERSION_NAME"
    echo "signing cert   SHA-256 $CERT"
    echo "built          $(date -u '+%Y-%m-%d %H:%M UTC')"
    echo
    echo "game           $(printf '%s\n' "$GAME_INFO" | sed -n 's/^package=//p'), sha256 $(printf '%s\n' "$GAME_INFO" | sed -n 's/^sha256=//p')"
    echo "ZettaBridge    $ZETTA_COMMIT"
    for patch in "$ROOT"/patches/zettabridge/*.patch; do
        echo "  + $(basename "$patch") sha256 $(sha256sum "$patch" | cut -d' ' -f1)"
    done
    echo "Dynarmic       $DYNARMIC_COMMIT + ZettaBridge's third_party/patches"
    for patch in "$ROOT"/patches/dynarmic/*.patch; do
        echo "  + $(basename "$patch") sha256 $(sha256sum "$patch" | cut -d' ' -f1)"
    done
    echo "NDK            $NDK_VERSION"
    echo "Boost headers  $BOOST_VERSION"
    echo
    echo "Install: adb install -r $NAME"
    echo "It updates an earlier install signed with the same key and keeps its worlds."
    echo "Built from your own copy of the game: for your own devices, not for distribution."
} > "$DEST/BUILD-INFO.txt"
ls -la "$DEST"

if [ "$INSTALL" = 1 ]; then
    step "install"
    "$SDK/platform-tools/adb.exe" install -r "$(cygpath -m "$DEST/$NAME")"
fi

step "done"
echo "release $RELEASE: $DEST/$NAME"
if [ "$NEW_KEY" = 1 ]; then
    echo "A new signing key was created in ~/.mcpe-arm64. Back it up: updates only install over"
    echo "builds signed with the same key."
fi
