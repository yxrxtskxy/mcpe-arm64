package com.zettabridge.launcher;

import android.Manifest;
import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.widget.Toast;

import java.util.Arrays;

/**
 * Answers a plugin's request for READ/WRITE_EXTERNAL_STORAGE through "All files access"
 * (GuestRuntime.route). Apps built before scoped storage ask for those permissions to use shared
 * storage by path: Minecraft PE keeps its worlds in /storage/emulated/0/games/com.mojang when its
 * storage location is External. A host targeting Android 11+ is never granted them, but with
 * MANAGE_EXTERNAL_STORAGE it can do everything they stood for. So when the host declares that
 * permission, a storage request opens its settings page, and the plugin is told whether the user
 * allowed it; PluginContext reports both permissions as granted from then on (grants).
 */
public final class StorageAccessActivity extends Activity {
    // Activity.requestPermissions sends this intent and reads the answer from these extras
    // (PackageManager.ACTION_REQUEST_PERMISSIONS and EXTRA_REQUEST_PERMISSIONS_*, system API).
    private static final String ACTION_REQUEST_PERMISSIONS = "android.content.pm.action.REQUEST_PERMISSIONS";
    private static final String EXTRA_NAMES = "android.content.pm.extra.REQUEST_PERMISSIONS_NAMES";
    private static final String EXTRA_RESULTS = "android.content.pm.extra.REQUEST_PERMISSIONS_RESULTS";

    private boolean openedSettings;
    private boolean paused;
    private boolean answered;

    /** READ/WRITE_EXTERNAL_STORAGE, while the host has "All files access". */
    static boolean grants(String permission) {
        return Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && isStorage(permission)
                && Environment.isExternalStorageManager();
    }

    /** A request for storage permissions only, from a host that declares MANAGE_EXTERNAL_STORAGE. */
    static boolean handles(Context host, Intent intent) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R || !ACTION_REQUEST_PERMISSIONS.equals(intent.getAction())) {
            return false;
        }
        String[] names = intent.getStringArrayExtra(EXTRA_NAMES);
        if (names == null || names.length == 0) return false;
        for (String name : names) {
            if (!isStorage(name)) return false;
        }
        return declaresAllFilesAccess(host);
    }

    static Intent intent(Context host, Intent request) {
        return new Intent(host, StorageAccessActivity.class)
                .putExtra(EXTRA_NAMES, request.getStringArrayExtra(EXTRA_NAMES));
    }

    private static boolean isStorage(String permission) {
        return Manifest.permission.READ_EXTERNAL_STORAGE.equals(permission)
                || Manifest.permission.WRITE_EXTERNAL_STORAGE.equals(permission);
    }

    private static boolean declaresAllFilesAccess(Context host) {
        try {
            String[] requested = host.getPackageManager()
                    .getPackageInfo(host.getPackageName(), PackageManager.GET_PERMISSIONS).requestedPermissions;
            return requested != null && Arrays.asList(requested).contains(Manifest.permission.MANAGE_EXTERNAL_STORAGE);
        } catch (PackageManager.NameNotFoundException e) {
            return false;
        }
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        // Recreated after the settings page was shown, or access is already there: answer now.
        if (state != null || Environment.isExternalStorageManager()) {
            answer();
            return;
        }
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.fromParts("package", getPackageName(), null)));
            openedSettings = true;
            Toast.makeText(this, "Allow \"All files access\" so the game can keep its files in shared storage",
                    Toast.LENGTH_LONG).show();
        } catch (ActivityNotFoundException e) {
            answer();
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        paused = true;
    }

    @Override
    protected void onResume() {
        super.onResume();
        // Back from the settings page, whether or not the user changed anything there.
        if (openedSettings && paused) answer();
    }

    /** Finishes with the answer Activity.requestPermissions expects: one result per permission. */
    private void answer() {
        if (answered) return;
        answered = true;
        String[] names = getIntent().getStringArrayExtra(EXTRA_NAMES);
        if (names == null) names = new String[0];
        int[] results = new int[names.length];
        for (int i = 0; i < names.length; i++) {
            results[i] = grants(names[i]) ? PackageManager.PERMISSION_GRANTED : PackageManager.PERMISSION_DENIED;
        }
        setResult(RESULT_OK, new Intent().putExtra(EXTRA_NAMES, names).putExtra(EXTRA_RESULTS, results));
        finish();
    }
}
