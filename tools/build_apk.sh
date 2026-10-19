#!/bin/sh
# Builds out/MinecraftPE-1.1.5-arm64.apk on Windows (Git Bash), end to end:
#   1. arm32 guest artifacts (stubs, zbhost, libzbjni, libzbcompat)   upstream tools/build_guest.sh
#   2. arm64 Android core (libzbridge.so, libzbproxy.so)               tools/build_android_core.sh
#   3. the runtime bundle (sysroot + guest + proxy)                   upstream tools/make_launcher_bundle.sh
#   4. app inputs: bundle, the game APK, its icon                     build/app-inputs
#   5. Gradle assembleRelease, then apksigner/aapt2 checks
# Prerequisites: tools/setup_toolchain_windows.sh, the sysroot (upstream tools/extract_sysroot.sh),
# Boost headers in .deps/boost-headers, ~/.gradle-dists/gradle-8.11.1 and tools/make_signing_key.sh.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ZB="$ROOT/upstream/ZettaBridge"
SDK=${ANDROID_SDK_ROOT:-$(cygpath -u "$LOCALAPPDATA")/Android/Sdk}
export NDK=${NDK:-$SDK/ndk/29.0.14206865}
export NDK_HOST=windows-x86_64
# Gradle 8.11 cannot run on the newer JDK this machine's JAVA_HOME points at; always use 17.
export JAVA_HOME=${MCPE_JAVA_HOME:-$(cygpath -m "$HOME/.jdks/temurin-17")}
export ANDROID_HOME="$(cygpath -m "$SDK")"
GRADLE=${GRADLE:-$HOME/.gradle-dists/gradle-8.11.1/bin/gradle}
BUILD_TOOLS="$SDK/build-tools/35.0.0"
GAME_APK=${GAME_APK:-$ROOT/game/minecraft-1.1.5.apk}
INPUTS="$ROOT/build/app-inputs"
OUT="$ROOT/out/MinecraftPE-1.1.5-arm64.apk"

step() { printf '\n=== %s\n' "$*"; }

[ -f "$GAME_APK" ] || { echo "missing game APK: $GAME_APK" >&2; exit 1; }
[ -f "$HOME/.mcpe-arm64/signing.properties" ] || { echo "run tools/make_signing_key.sh first" >&2; exit 1; }

step "guest artifacts"
(cd "$ZB" && sh tools/build_guest.sh >/dev/null)

step "arm64 Android core"
sh "$ROOT/tools/build_android_core.sh" | tail -2

step "runtime bundle"
(cd "$ZB" && sh tools/make_launcher_bundle.sh | tail -2)

step "app inputs"
rm -rf "$INPUTS"
mkdir -p "$INPUTS/assets/game"
cp -R "$ZB/build/launcher/assets/." "$INPUTS/assets/"
cp -R "$ZB/build/launcher/jniLibs" "$INPUTS/jniLibs"
cp "$GAME_APK" "$INPUTS/assets/game/base.apk"
GAME_SHA=$(sha256sum "$GAME_APK" | cut -d' ' -f1)
GAME_PACKAGE=$("$BUILD_TOOLS/aapt2.exe" dump packagename "$(cygpath -m "$GAME_APK")" | tr -d '\r')
printf 'package=%s\nsha256=%s\n' "$GAME_PACKAGE" "$GAME_SHA" > "$INPUTS/assets/game/info.properties"
# The launcher icon is the game's own, taken from its APK at build time.
for density in mdpi hdpi xhdpi xxhdpi xxxhdpi; do
    mkdir -p "$INPUTS/res/mipmap-$density"
    unzip -p "$GAME_APK" "res/drawable-$density-v4/icon.png" > "$INPUTS/res/mipmap-$density/ic_launcher.png"
done
echo "game: $GAME_PACKAGE sha256=$GAME_SHA"

# BUILD_TYPE=debug makes a debuggable APK (same key, installs over a release build) so that
# `adb shell run-as` can read the runtime report and the game's files while it is being fixed.
BUILD_TYPE=${BUILD_TYPE:-release}
case "$BUILD_TYPE" in
    release) TASK=assembleRelease ;;
    debug) TASK=assembleDebug ;;
    *) echo "BUILD_TYPE must be release or debug" >&2; exit 1 ;;
esac
step "gradle $TASK"
(cd "$ROOT/android" && "$GRADLE" --no-daemon -q "$TASK")
BUILT="$ROOT/android/app/build/outputs/apk/$BUILD_TYPE/app-$BUILD_TYPE.apk"

step "checks"
"$BUILD_TOOLS/zipalign.exe" -c -P 16 4 "$(cygpath -m "$BUILT")" && echo "zipalign: OK (16 KiB pages)"
"$BUILD_TOOLS/apksigner.bat" verify --print-certs "$(cygpath -m "$BUILT")" | head -3
"$BUILD_TOOLS/aapt2.exe" dump badging "$(cygpath -m "$BUILT")" | tr -d '\r' \
    | grep -E "^package|native-code|launchable-activity|sdkVersion|targetSdkVersion"
mkdir -p "$(dirname "$OUT")"
cp "$BUILT" "$OUT"
ls -la "$OUT"
sha256sum "$OUT"
