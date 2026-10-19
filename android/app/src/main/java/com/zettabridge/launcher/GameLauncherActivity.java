package com.zettabridge.launcher;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Properties;

/**
 * The single-game entry point, in the main process. The game APK ships inside this app's assets;
 * the first launch (and the first after an update that changes it) imports it as a plugin, which
 * extracts its arm32 libraries and applies the import-time ELF fixups. Every launch then goes
 * through the ordinary plugin route (PluginSwitchActivity, then GuestLaunchActivity in :guest),
 * exactly as a pinned shortcut of the multi-app launcher would.
 */
public class GameLauncherActivity extends Activity {
    private static final String TAG = "mcpe-arm64";
    /** Written by tools/build_apk.sh: package=..., sha256=... of the bundled APK. */
    private static final String GAME_INFO_ASSET = "game/info.properties";
    private static final String GAME_APK_ASSET = "game/base.apk";
    /**
     * In the plugin directory: the sha256 of the bundled APK it was imported from, and the
     * install time of the launcher that imported it. The import-time ELF fixups live in this
     * APK's libzbridge, so an update re-imports even when the game itself is unchanged; the
     * plugin's data directory, worlds included, is kept.
     */
    private static final String IMPORTED_FROM = "embedded-sha256";

    private String packageName;
    private String importKey;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        try {
            Properties info = new Properties();
            try (InputStream in = getAssets().open(GAME_INFO_ASSET)) {
                info.load(in);
            }
            packageName = info.getProperty("package");
            String bundledSha256 = info.getProperty("sha256");
            if (packageName == null || bundledSha256 == null) throw new IOException(GAME_INFO_ASSET + " is incomplete");
            long installed = getPackageManager().getPackageInfo(getPackageName(), 0).lastUpdateTime;
            importKey = bundledSha256 + " " + installed;
        } catch (IOException | PackageManager.NameNotFoundException e) {
            showError(Diagnostics.report(this, "this build has no bundled game", e, false));
            return;
        }
        PluginRecord record = PluginStore.find(this, packageName);
        if (record != null && record.isLaunchable() && importKey.equals(importedFrom(record))) {
            launch();
            return;
        }
        prepare();
    }

    private void prepare() {
        showProgress();
        new Thread(() -> {
            try {
                // A guest still running from an older import keeps the old APK mapped.
                if (ActivePlugin.guestIsRunning(this)) ActivePlugin.endGuestProcess(this);
                PluginRecord imported;
                try (InputStream in = getAssets().open(GAME_APK_ASSET, AssetManager.ACCESS_STREAMING)) {
                    imported = PluginStore.importApk(this, in);
                }
                if (!packageName.equals(imported.packageName)) {
                    throw new IOException("the bundled APK is " + imported.packageName + ", not " + packageName);
                }
                if (!imported.isLaunchable()) throw new IOException("the bundled game is not launchable: " + imported.status());
                writeImportedFrom(imported, importKey);
                Log.i(TAG, "imported " + imported.packageName + " " + imported.versionName + " (" + imported.status() + ")");
                runOnUiThread(this::launch);
            } catch (Exception e) {
                String detail = Diagnostics.report(this, "cannot prepare " + packageName, e, false);
                runOnUiThread(() -> showError(detail));
            }
        }, "mcpe-import").start();
    }

    private void launch() {
        startActivity(PluginSwitchActivity.intent(this, packageName));
        finish();
        overridePendingTransition(0, 0);
    }

    private static String importedFrom(PluginRecord record) {
        File marker = new File(record.dir, IMPORTED_FROM);
        if (!marker.isFile()) return null;
        try {
            return new String(Files.readAllBytes(marker.toPath()), StandardCharsets.US_ASCII).trim();
        } catch (IOException e) {
            return null;
        }
    }

    private static void writeImportedFrom(PluginRecord record, String key) throws IOException {
        File marker = new File(record.dir, IMPORTED_FROM);
        File tmp = new File(record.dir, IMPORTED_FROM + ".tmp");
        try (FileOutputStream out = new FileOutputStream(tmp)) {
            out.write((key + "\n").getBytes(StandardCharsets.US_ASCII));
            out.getFD().sync();
        }
        if (!tmp.renameTo(marker)) throw new IOException("cannot write " + marker);
    }

    private void showProgress() {
        LinearLayout root = column();
        TextView title = text(getString(getApplicationInfo().labelRes), 22);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        root.addView(title);
        root.addView(text("Preparing the game for this phone (first launch only)...", 15));
        ProgressBar progress = new ProgressBar(this);
        progress.setIndeterminate(true);
        root.addView(progress);
        setContentView(root);
    }

    private void showError(String detail) {
        LinearLayout root = column();
        root.setGravity(Gravity.START);
        TextView title = text("The game could not be started", 20);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        root.addView(title);
        TextView body = text(detail, 12);
        body.setTypeface(Typeface.MONOSPACE);
        body.setGravity(Gravity.START);
        body.setTextIsSelectable(true);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(body);
        root.addView(scroll, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        buttons.addView(button("Retry", v -> recreate()));
        buttons.addView(button("Copy", v -> Diagnostics.copy(this, "Minecraft PE arm64 error", detail)));
        buttons.addView(button("Runtime tools", v -> startActivity(new Intent(this, LibraryActivity.class))));
        root.addView(buttons);
        setContentView(root);
    }

    private LinearLayout column() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);
        int pad = dp(24);
        root.setPadding(pad, pad, pad, pad);
        root.setBackgroundColor(Color.BLACK);
        root.setFitsSystemWindows(true);
        return root;
    }

    private TextView text(String value, int sp) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextColor(Color.WHITE);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        view.setGravity(Gravity.CENTER);
        view.setPadding(0, dp(8), 0, dp(8));
        return view;
    }

    private Button button(String label, View.OnClickListener listener) {
        Button button = new Button(this);
        button.setText(label);
        button.setOnClickListener(listener);
        return button;
    }

    private int dp(int value) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, value, getResources().getDisplayMetrics());
    }
}
