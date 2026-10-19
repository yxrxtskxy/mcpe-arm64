#!/bin/sh
# Recreates upstream/ZettaBridge from scratch: the pinned ZettaBridge commit, its Dynarmic
# submodule at the commit it records, ZettaBridge's own two Dynarmic patches, and this project's
# changes (patches/dynarmic/*.patch on top of those, then patches/zettabridge/*.patch). Refuses to
# touch an existing checkout.
#
# Afterwards, the arm32 sysroot: (cd upstream/ZettaBridge && sh tools/extract_sysroot.sh)
# (downloads a ~1.2 GB AOSP GSI; needs debugfs, so run it in WSL or Linux).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEST=${DEST:-$ROOT/upstream/ZettaBridge}
REPOSITORY=https://github.com/ZailoxTT/ZettaBridge.git
ZETTA_COMMIT=d6066b9b9e61de27041ba6e1f3c499c080c9e2b7
DYNARMIC_COMMIT=86458a0bd369d63ba4c2ef812cacbb6c9080c065

# LF checkouts everywhere, submodules included: Git for Windows defaults to CRLF, which breaks the
# assembly and shell sources. Environment config reaches every nested git process.
export GIT_CONFIG_COUNT=2 GIT_CONFIG_KEY_0=core.autocrlf GIT_CONFIG_VALUE_0=false \
    GIT_CONFIG_KEY_1=core.eol GIT_CONFIG_VALUE_1=lf

if [ -e "$DEST" ]; then
    echo "$DEST exists; move it away first" >&2
    exit 1
fi
mkdir -p "$DEST"
git -C "$DEST" init -q
git -C "$DEST" config core.autocrlf false
git -C "$DEST" remote add origin "$REPOSITORY"
git -C "$DEST" fetch -q --depth=1 origin "$ZETTA_COMMIT"
git -C "$DEST" checkout -q --detach FETCH_HEAD
[ "$(git -C "$DEST" rev-parse HEAD)" = "$ZETTA_COMMIT" ] || { echo "ZettaBridge revision mismatch" >&2; exit 1; }
git -C "$DEST" -c core.autocrlf=false submodule update -q --init --recursive
[ "$(git -C "$DEST/third_party/dynarmic" rev-parse HEAD)" = "$DYNARMIC_COMMIT" ] || { echo "Dynarmic revision mismatch" >&2; exit 1; }
git -C "$DEST/third_party/dynarmic" config core.autocrlf false
git -C "$DEST/third_party/dynarmic" apply ../patches/dynarmic-0001-thumb32-armv8.patch ../patches/dynarmic-0002-asimd-narrowing.patch
for patch in "$ROOT"/patches/dynarmic/*.patch; do
    git -C "$DEST/third_party/dynarmic" apply --whitespace=nowarn "$patch"
    echo "applied dynarmic/$(basename "$patch")"
done
git -C "$DEST" checkout -q -b mcpe-arm64
for patch in "$ROOT"/patches/zettabridge/*.patch; do
    git -C "$DEST" apply --whitespace=nowarn "$patch"
    echo "applied $(basename "$patch")"
done
echo "ZettaBridge ready in $DEST"
