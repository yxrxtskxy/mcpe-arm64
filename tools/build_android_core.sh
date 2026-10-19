#!/bin/sh
# Builds the arm64 Android core on Windows (Git Bash): libzbridge.so (translator + JNI bridge)
# and libzbproxy.so (the per-library proxy ART loads), with the Windows NDK r29.
# Output: upstream/ZettaBridge/build/android-arm64/core/, where make_launcher_bundle.sh looks.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ZB="$ROOT/upstream/ZettaBridge"
NDK=${NDK:-$(cygpath -u "$LOCALAPPDATA")/Android/Sdk/ndk/29.0.14206865}
BOOST="$ROOT/.deps/boost-headers"
OUT="$ZB/build/android-arm64"

[ -f "$BOOST/boost/version.hpp" ] || { echo "missing Boost headers in $BOOST" >&2; exit 1; }
cmake -S "$(cygpath -m "$ZB")" -B "$(cygpath -m "$OUT")" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$(cygpath -m "$NDK")/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DCMAKE_BUILD_TYPE=Release \
    -DZB_BUILD_TESTS=OFF -DBoost_INCLUDE_DIR="$(cygpath -m "$BOOST")"
ninja -C "$(cygpath -m "$OUT")" zbridge zbproxy
ls -la "$OUT/core/libzbridge.so" "$OUT/core/libzbproxy.so"
