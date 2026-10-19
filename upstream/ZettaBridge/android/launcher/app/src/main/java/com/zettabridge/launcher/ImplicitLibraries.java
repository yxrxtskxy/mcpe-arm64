package com.zettabridge.launcher;

import android.util.Log;

import dalvik.system.PathClassLoader;

import java.io.File;

/**
 * The shared libraries Android adds on its own to the class path of an app with an old
 * targetSdk, given to a plugin the same way.
 *
 * <p>The package manager does this for installed apps (OrgApacheHttpLegacyUpdater,
 * AndroidTestBaseUpdater): the Apache HTTP client left the boot class path in API 28 and the
 * android.test classes in API 29, and an app built against the older SDK still finds them. A
 * plugin is not installed, so nothing adds them, and its first use of one fails with
 * ClassNotFoundException; Minecraft PE's Xbox Live code reaches org.apache.http.Header from its
 * JNI_OnLoad, and the game aborts on the JNI call that follows.
 */
final class ImplicitLibraries {
    private static final String TAG = "zb-launcher";

    private static final class Library {
        final String path;
        final int firstSdkWithout;

        Library(String path, int firstSdkWithout) {
            this.path = path;
            this.firstSdkWithout = firstSdkWithout;
        }
    }

    private static final Library[] LIBRARIES = {
        new Library("/system/framework/org.apache.http.legacy.jar", 28),
        new Library("/system/framework/android.test.base.jar", 29),
    };

    private ImplicitLibraries() {}

    /**
     * {@code platform} itself when the plugin needs none of them, otherwise a loader for the
     * system jars it needs whose parent is {@code platform}: the plugin's own classes still come
     * first (PluginClassLoader is child-first), these are found only where the plugin has none.
     */
    static ClassLoader parentFor(int targetSdk, ClassLoader platform) {
        StringBuilder path = new StringBuilder();
        for (Library library : LIBRARIES) {
            if (targetSdk >= library.firstSdkWithout || !new File(library.path).isFile()) continue;
            if (path.length() > 0) path.append(File.pathSeparatorChar);
            path.append(library.path);
        }
        if (path.length() == 0) return platform;
        Log.i(TAG, "implicit shared libraries for targetSdk " + targetSdk + ": " + path);
        return new PathClassLoader(path.toString(), platform);
    }
}
