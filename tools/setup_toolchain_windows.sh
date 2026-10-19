#!/bin/sh
# Installs the Windows-side build toolchain (run from Git Bash):
#   Android SDK pieces + NDK r29 in %LOCALAPPDATA%\Android\Sdk (Android Studio's default location)
#   Temurin JDK 17 in ~/.jdks/temurin-17 (Gradle 8.11 cannot run on newer JDKs)
#   Gradle 8.11.1 in ~/.gradle-dists/gradle-8.11.1 (tools/build_apk.sh runs it from there)
# Every archive is verified against the checksum its publisher lists. Re-running skips what is
# already installed.
#
# Why Windows and not WSL: this machine only has WSL1, and the NDK r29 Linux clang binaries are
# BOLT-optimized with a 2 MiB-aligned extra PT_LOAD that the WSL1 ELF loader rejects
# ("Exec format error").
set -eu

SDK=${ANDROID_SDK_ROOT:-$(cygpath -u "$LOCALAPPDATA")/Android/Sdk}
JDK_DIR=${JDK_DIR:-$HOME/.jdks/temurin-17}
CACHE=${CACHE:-$HOME/.cache/mcpe-arm64/downloads}
BASE=https://dl.google.com/android/repository
mkdir -p "$SDK" "$CACHE"

fetch() {  # url file algo sum
    if [ ! -f "$CACHE/$2" ]; then
        echo "[setup] downloading $2"
        curl -fL --retry 3 -o "$CACHE/$2.part" "$1"
        mv "$CACHE/$2.part" "$CACHE/$2"
    fi
    echo "$4  $CACHE/$2" | "$3sum" -c --quiet
}

install_zip() {  # name archive sha1 dest
    if [ -d "$SDK/$4" ] && [ -n "$(ls -A "$SDK/$4" 2>/dev/null)" ]; then
        echo "[setup] $1: already installed"
        return
    fi
    fetch "$BASE/$2" "$2" sha1 "$3"
    tmp="$SDK/.extract-$1"
    rm -rf "$tmp"
    mkdir -p "$tmp"
    unzip -q "$CACHE/$2" -d "$tmp"
    inner="$tmp/$(ls "$tmp" | head -1)"
    mkdir -p "$(dirname "$SDK/$4")"
    rm -rf "$SDK/$4"
    mv "$inner" "$SDK/$4"
    rm -rf "$tmp"
    echo "[setup] $1: installed to $SDK/$4"
}

install_zip platform-tools platform-tools_r37.0.1-win.zip e03e78b1d80b396f1c3358e31251cb31740e1110 platform-tools
install_zip build-tools build-tools_r35_windows.zip af059bb67cf7786f45ee0db85e2d24985df1b4b6 build-tools/35.0.0
install_zip platform-35 platform-35_r02.zip 0bb560a90a7a2cbd0dd8348224d518b638fe7949 platforms/android-35
install_zip ndk-r29 android-ndk-r29-windows.zip ab3bb30fbb9e6903666d60c55d11e78b04e07472 ndk/29.0.14206865

if [ -x "$JDK_DIR/bin/java.exe" ]; then
    echo "[setup] jdk17: already installed"
else
    JDK_ZIP=OpenJDK17U-jdk_x64_windows_hotspot_17.0.20.1_1.zip
    fetch "https://github.com/adoptium/temurin17-binaries/releases/download/jdk-17.0.20.1%2B1/$JDK_ZIP" \
        "$JDK_ZIP" sha256 e53a79c3c3d86865bd7e787903884331068e71321714ffd44f145785affc7cb0
    tmp="$JDK_DIR.extract"
    rm -rf "$tmp"
    mkdir -p "$tmp" "$(dirname "$JDK_DIR")"
    unzip -q "$CACHE/$JDK_ZIP" -d "$tmp"
    mv "$tmp/$(ls "$tmp" | head -1)" "$JDK_DIR"
    rm -rf "$tmp"
    echo "[setup] jdk17: installed to $JDK_DIR"
fi

GRADLE_DIR=${GRADLE_DIR:-$HOME/.gradle-dists/gradle-8.11.1}
if [ -f "$GRADLE_DIR/bin/gradle" ]; then
    echo "[setup] gradle: already installed"
else
    GRADLE_ZIP=gradle-8.11.1-bin.zip
    fetch "https://services.gradle.org/distributions/$GRADLE_ZIP" \
        "$GRADLE_ZIP" sha256 f397b287023acdba1e9f6fc5ea72d22dd63669d59ed4a289a29b1a76eee151c6
    tmp="$GRADLE_DIR.extract"
    rm -rf "$tmp"
    mkdir -p "$tmp" "$(dirname "$GRADLE_DIR")"
    unzip -q "$CACHE/$GRADLE_ZIP" -d "$tmp"
    mv "$tmp/$(ls "$tmp" | head -1)" "$GRADLE_DIR"
    rm -rf "$tmp"
    echo "[setup] gradle: installed to $GRADLE_DIR"
fi
echo "[setup] done: SDK=$SDK JDK=$JDK_DIR Gradle=$GRADLE_DIR"
