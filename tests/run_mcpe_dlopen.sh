#!/bin/sh
# Local load test (WSL, x86_64 zbrun): Minecraft PE's arm32 libraries through the real Android
# linker, after the same import fixups the phone applies. Needs tools/wsl_build_host_x64.sh and
# the guest build (build/guest) first; the test program is built by build_guest_tests.sh.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ZB="$ROOT/upstream/ZettaBridge"
BIN=${BIN:-$HOME/.cache/mcpe-arm64/build/host-x64/cli}
APK=${APK:-$ROOT/game/minecraft-1.1.5.apk}
WORK=${WORK:-$HOME/.cache/mcpe-arm64/mcpe-libs}
rm -rf "$WORK"
mkdir -p "$WORK"
unzip -ojq "$APK" 'lib/armeabi-v7a/*' -d "$WORK"
"$BIN/zbfix/zbfix" "$WORK"/*.so
"$BIN/zbrun/zbrun" --sysroot "$ZB/sysroot" --env "LD_LIBRARY_PATH=$ZB/build/guest/lib" \
    "$ROOT/build/guest-tests/mcpe_dlopen" "$WORK" 24
