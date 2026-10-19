// Minecraft PE 1.1.5 for arm64-only phones: the ZettaBridge launcher (Java sources used in place
// from upstream/ZettaBridge) with the game APK bundled as its one plugin. Plain Java, no AndroidX.
//
// Generated inputs come only from build/app-inputs, written by tools/build_apk.sh:
//   assets/zb/...            the arm32 runtime bundle (sysroot, guest stubs, zbhost, proxy)
//   assets/game/base.apk     the original game APK, unmodified
//   assets/game/info.properties
//   jniLibs/arm64-v8a/libzbridge.so
//   res/mipmap-*/ic_launcher.png, taken from the game APK
import java.io.File
import java.util.Properties

plugins {
    id("com.android.application")
}

val zettaBridge = rootProject.file("../upstream/ZettaBridge")
val appInputs = rootProject.file("../build/app-inputs")

// The signing key lives outside the repository: ~/.mcpe-arm64/signing.properties names the
// keystore and its passwords (tools/make_signing_key.sh creates both). Keep that keystore: an
// update only installs over a build signed with the same key.
val signingProps = Properties()
val signingPropsFile = File(System.getProperty("user.home"), ".mcpe-arm64/signing.properties")
if (signingPropsFile.isFile) {
    signingPropsFile.inputStream().use { stream -> signingProps.load(stream) }
}
val keystore: File? = signingProps.getProperty("storeFile")?.let { path -> File(path) }
val hasKey = keystore != null && keystore.isFile

android {
    namespace = "com.zettabridge.launcher"
    compileSdk = 35
    buildToolsVersion = "35.0.0"

    defaultConfig {
        // Keep in sync with android:targetPackage in res/xml/shortcuts.xml.
        applicationId = "com.mcpe115.arm64"
        minSdk = 26
        targetSdk = 35
        versionCode = 2
        versionName = "1.1.5-arm64.2"
        ndk { abiFilters += "arm64-v8a" }
    }

    signingConfigs {
        if (hasKey) {
            create("mcpe") {
                storeFile = keystore
                storePassword = signingProps.getProperty("storePassword")
                keyAlias = signingProps.getProperty("keyAlias")
                keyPassword = signingProps.getProperty("keyPassword")
            }
        }
    }
    buildTypes {
        release {
            isMinifyEnabled = false
            if (hasKey) signingConfig = signingConfigs.getByName("mcpe")
        }
        // Same key as release, so either build installs over the other and keeps the worlds.
        debug {
            if (hasKey) signingConfig = signingConfigs.getByName("mcpe")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    // The bundled game APK is already compressed and is streamed into the plugin store.
    androidResources {
        noCompress += "apk"
    }
    sourceSets.getByName("main") {
        java.srcDir(File(zettaBridge, "android/launcher/app/src/main/java"))
        assets.srcDir(File(appInputs, "assets"))
        jniLibs.srcDir(File(appInputs, "jniLibs"))
        res.srcDir(File(appInputs, "res"))
    }
}

dependencies {
    // Apache-2.0. Lifts hidden API restrictions for the framework hooks in the :guest process.
    implementation("org.lsposed.hiddenapibypass:hiddenapibypass:6.1")
}
