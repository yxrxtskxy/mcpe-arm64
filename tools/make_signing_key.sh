#!/bin/sh
# Creates this project's APK signing key, once, outside the repository:
#   ~/.mcpe-arm64/release.jks and ~/.mcpe-arm64/signing.properties (random passwords).
# Back both up: Android only installs an update over a build signed with the same key, and a new
# key means uninstalling the game first (which deletes worlds kept in its private storage).
set -eu
DIR="$HOME/.mcpe-arm64"
KEYSTORE="$DIR/release.jks"
PROPS="$DIR/signing.properties"
JAVA_HOME=${JAVA_HOME:-$HOME/.jdks/temurin-17}
if [ -f "$KEYSTORE" ] || [ -f "$PROPS" ]; then
    echo "signing key already exists in $DIR; not touching it"
    exit 0
fi
mkdir -p "$DIR"
PASSWORD=$(od -An -N24 -tx1 /dev/urandom | tr -d ' \n')
"$JAVA_HOME/bin/keytool" -genkeypair -keystore "$(cygpath -m "$KEYSTORE" 2>/dev/null || echo "$KEYSTORE")" \
    -storetype PKCS12 -alias mcpe-arm64 -keyalg RSA -keysize 3072 -validity 10000 \
    -storepass "$PASSWORD" -keypass "$PASSWORD" -dname "CN=Minecraft PE arm64 local build" >/dev/null
{
    echo "storeFile=$(cygpath -m "$KEYSTORE" 2>/dev/null || echo "$KEYSTORE")"
    echo "storePassword=$PASSWORD"
    echo "keyAlias=mcpe-arm64"
    echo "keyPassword=$PASSWORD"
} > "$PROPS"
chmod 600 "$PROPS" "$KEYSTORE" 2>/dev/null || true
echo "created $KEYSTORE and $PROPS"
