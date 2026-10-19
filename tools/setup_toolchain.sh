#!/bin/sh
# Installs the Android SDK pieces and NDK r29 used by this project into $SDK (WSL / Linux x86_64).
# Every archive is checked against the SHA-1 published in Google's repository2-3.xml.
# Re-running is safe: installed components are skipped.
set -eu

SDK=${ANDROID_SDK_ROOT:-$HOME/android-sdk}
CACHE=${CACHE:-$HOME/.cache/mcpe-arm64/downloads}
BASE=https://dl.google.com/android/repository
mkdir -p "$SDK" "$CACHE"

# name|archive|sha1|destination (relative to $SDK)|top-level directory inside the zip
COMPONENTS="
cmdline-tools|commandlinetools-linux-16111833_latest.zip|e025545c62a8e64c7559119566a569fb1dec5f60|cmdline-tools/latest|cmdline-tools
platform-tools|platform-tools_r37.0.1-linux.zip|477254aa5f903c15cf51001717bdf347fb6b53e0|platform-tools|platform-tools
build-tools|build-tools_r35_linux.zip|2cfaa0bbb2336e9ec18ed3ecea84fa2e2af607bc|build-tools/35.0.0|android-15
platform-35|platform-35_r02.zip|0bb560a90a7a2cbd0dd8348224d518b638fe7949|platforms/android-35|android-35
ndk-r29|android-ndk-r29-linux.zip|87e2bb7e9be5d6a1c6cdf5ec40dd4e0c6d07c30b|ndk/29.0.14206865|android-ndk-r29
"

install_component() {
    name=$1 archive=$2 sha1=$3 dest=$4 top=$5
    if [ -d "$SDK/$dest" ] && [ -n "$(ls -A "$SDK/$dest" 2>/dev/null)" ]; then
        echo "[setup] $name: already installed"
        return
    fi
    if [ ! -f "$CACHE/$archive" ]; then
        echo "[setup] $name: downloading $archive"
        curl -fL --retry 3 -o "$CACHE/$archive.part" "$BASE/$archive"
        mv "$CACHE/$archive.part" "$CACHE/$archive"
    fi
    echo "$sha1  $CACHE/$archive" | sha1sum -c --quiet
    tmp="$SDK/.extract-$name"
    rm -rf "$tmp"
    mkdir -p "$tmp"
    unzip -q "$CACHE/$archive" -d "$tmp"
    # The zip's single top-level directory name differs between components and releases.
    inner="$tmp/$top"
    [ -d "$inner" ] || inner="$tmp/$(ls "$tmp" | head -1)"
    mkdir -p "$(dirname "$SDK/$dest")"
    rm -rf "$SDK/$dest"
    mv "$inner" "$SDK/$dest"
    rm -rf "$tmp"
    echo "[setup] $name: installed to $SDK/$dest"
}

echo "$COMPONENTS" | while IFS='|' read -r name archive sha1 dest top; do
    [ -n "$name" ] || continue
    install_component "$name" "$archive" "$sha1" "$dest" "$top"
done

# ZettaBridge's scripts look for the NDK at ~/android-ndk-r29.
ln -sfn "$SDK/ndk/29.0.14206865" "$HOME/android-ndk-r29"
echo "[setup] done: SDK=$SDK NDK=$HOME/android-ndk-r29"
