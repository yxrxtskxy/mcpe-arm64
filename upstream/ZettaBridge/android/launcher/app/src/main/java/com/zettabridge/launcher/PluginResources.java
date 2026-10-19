package com.zettabridge.launcher;

import android.content.res.Resources;
import android.util.Log;

import java.lang.reflect.Method;

/**
 * The plugin's Resources as plugin code sees them, with one difference: a lookup by name in the
 * launcher's package is a lookup in the plugin's own package.
 *
 * <p>PluginContext keeps the launcher's identity (system services check the package against the
 * uid), so plugin code that asks {@code getResources().getIdentifier(name, type,
 * getPackageName())} names the launcher's package, and the plugin's resource table has no such
 * package: the id comes back 0. Minecraft PE's Xbox Live code reads its raw "xboxservices" config
 * exactly that way and fails with Resources$NotFoundException for #0x0.
 *
 * <p>Everything else is the original object's: this subclass shares its ResourcesImpl (hidden
 * getImpl/setImpl; the :guest process is exempt from hidden API checks) and takes it again on
 * every {@link #sync}, so configuration changes the framework applies to the original reach it.
 * Without the hidden methods the original is used unchanged.
 */
final class PluginResources extends Resources {
    private static final String TAG = "zb-launcher";
    private static Method getImpl;
    private static Method setImpl;
    private static boolean reflectionFailed;

    private final Resources base;
    private final String hostPackage;
    private final String pluginPackage;
    private Object impl;

    @SuppressWarnings("deprecation")
    private PluginResources(Resources base, String hostPackage, String pluginPackage) {
        super(base.getAssets(), base.getDisplayMetrics(), base.getConfiguration());
        this.base = base;
        this.hostPackage = hostPackage;
        this.pluginPackage = pluginPackage;
    }

    /** {@code base} with the launcher package mapped to the plugin's, or {@code base} itself. */
    static Resources wrap(Resources base, String hostPackage, String pluginPackage) {
        if (base == null || hostPackage.equals(pluginPackage) || !reflection()) return base;
        PluginResources wrapped = new PluginResources(base, hostPackage, pluginPackage);
        return wrapped.syncImpl() ? wrapped : base;
    }

    /** The resources to hand out now: a wrapper re-takes its original's current ResourcesImpl. */
    static Resources sync(Resources resources) {
        if (resources instanceof PluginResources) {
            PluginResources wrapped = (PluginResources) resources;
            if (!wrapped.syncImpl()) return wrapped.base;
        }
        return resources;
    }

    private static synchronized boolean reflection() {
        if (getImpl != null) return true;
        if (reflectionFailed) return false;
        try {
            Method get = Resources.class.getDeclaredMethod("getImpl");
            Class<?> implClass = get.getReturnType();
            Method set = Resources.class.getDeclaredMethod("setImpl", implClass);
            get.setAccessible(true);
            set.setAccessible(true);
            getImpl = get;
            setImpl = set;
            return true;
        } catch (ReflectiveOperationException | RuntimeException e) {
            reflectionFailed = true;
            Log.w(TAG, "plugin resource lookups keep the launcher package: " + e);
            return false;
        }
    }

    private boolean syncImpl() {
        try {
            Object current = getImpl.invoke(base);
            if (current != impl) {
                setImpl.invoke(this, current);
                impl = current;
            }
            return true;
        } catch (ReflectiveOperationException | RuntimeException e) {
            Log.w(TAG, "cannot share the plugin's ResourcesImpl: " + e);
            return false;
        }
    }

    @Override
    public int getIdentifier(String name, String defType, String defPackage) {
        if (hostPackage.equals(defPackage)) defPackage = pluginPackage;
        // A fully qualified "package:type/name" carries the package in the name itself.
        if (name != null && name.startsWith(hostPackage + ":")) {
            name = pluginPackage + name.substring(hostPackage.length());
        }
        return super.getIdentifier(name, defType, defPackage);
    }
}
