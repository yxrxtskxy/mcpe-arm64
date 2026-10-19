#!/bin/sh
# x86_64 Linux test build of zbrun/zbfix (WSL). Used only to run guest programs on this PC:
# Dynarmic's x64 backend executes the arm32 guest, so the real Android linker can load the
# game's libraries here without a phone. Not shipped.
set -eu
ZB=${ZB:-/mnt/c/Server/mcpe-arm64/upstream/ZettaBridge}
OUT=${OUT:-$HOME/.cache/mcpe-arm64/build/host-x64}
cmake -S "$ZB" -B "$OUT" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DZB_BUILD_TESTS=OFF
ninja -C "$OUT" zbrun zbfix
ls -la "$OUT/cli/zbrun/zbrun" "$OUT/cli/zbfix/zbfix"
