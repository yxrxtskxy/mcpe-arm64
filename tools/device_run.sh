#!/bin/sh
# One device iteration (Git Bash, phone on adb): install out/MinecraftPE-1.1.5-arm64.apk over the
# existing install (keeps worlds), launch it, wait, then save the whole log to logs/ and print the
# lines that matter: our tags, the game's, ART/JNI aborts and native crashes.
#   tools/device_run.sh [seconds to wait, default 25] [--no-install]
set -eu
# Device paths (/sdcard/...) must reach adb untouched; Git Bash would rewrite them.
export MSYS_NO_PATHCONV=1
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ADB=${ADB:-$(cygpath -u "$LOCALAPPDATA")/Android/Sdk/platform-tools/adb.exe}
PACKAGE=com.yxrxtskxy.mcpe.arm64
WAIT=${1:-25}
APK="$ROOT/out/MinecraftPE-1.1.5-arm64.apk"
LOG="$ROOT/logs/device-$(date +%Y%m%d-%H%M%S).log"
mkdir -p "$ROOT/logs"

if [ "${2:-}" != "--no-install" ]; then
    "$ADB" install -r "$(cygpath -m "$APK")" | tail -1
fi
"$ADB" shell am force-stop "$PACKAGE"
"$ADB" logcat -c
"$ADB" logcat -b crash -c 2>/dev/null || true
"$ADB" shell am start -n "$PACKAGE/com.zettabridge.launcher.GameLauncherActivity" | tail -1
sleep "$WAIT"
"$ADB" logcat -d -b main -b system -b crash -v threadtime > "$LOG"
echo "full log: $LOG"
PIDS=$("$ADB" shell pidof "$PACKAGE" "$PACKAGE:guest" 2>/dev/null | tr -d '\r' || true)
echo "running pids: ${PIDS:-none}"
# The translator's runtime report (debuggable builds only: run-as needs them).
REPORT="${LOG%.log}-report.txt"
"$ADB" shell run-as "$PACKAGE" cat "files/zb-runtime-report.txt" > "$REPORT" 2>/dev/null \
    && echo "runtime report: $REPORT" || rm -f "$REPORT"
grep -E " (zbridge|zb-launcher|mcpe-arm64|MinecraftPE|MCPE|FMOD|fmod|libc|DEBUG|AndroidRuntime) *:| F [^ ]+ *: |JNI DETECTED|Pending exception|java\.lang\.[A-Za-z]+(Error|Exception)" "$LOG" \
    | grep -v "F DEBUG   :       #[0-9]" | tail -${LINES_SHOWN:-80}
