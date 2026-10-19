package com.zettabridge.launcher;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.util.Log;
import android.widget.Toast;

import java.io.IOException;

/**
 * Stands in for the gallery when a plugin starts ACTION_PICK on images (GuestRuntime.route).
 * Old apps read the picked image's path from the MediaStore "_data" column and open the file
 * themselves; Minecraft PE's custom skins do. Under scoped storage that path is often unreadable,
 * Photos and document providers have no such column, and a phone may have no app for ACTION_PICK
 * at all. This opens the system file picker instead, copies the chosen image into the launcher's
 * cache and answers with a PickedImageProvider URI, whose "_data" is the copy.
 */
public final class ImagePickActivity extends Activity {
    private static final String TAG = "zb-launcher";
    private static final int REQUEST_PICK = 1;

    /** An ACTION_PICK for an image: by MIME type, or by a MediaStore images collection as data. */
    static boolean handles(Intent intent) {
        if (!Intent.ACTION_PICK.equals(intent.getAction()) || intent.getComponent() != null) return false;
        String type = intent.getType();
        if (type != null) return type.startsWith("image/") || type.equals("vnd.android.cursor.dir/image");
        Uri data = intent.getData();
        return data != null && "media".equals(data.getAuthority()) && data.getPathSegments().contains("images");
    }

    static Intent intent(Context host) {
        return new Intent(host, ImagePickActivity.class);
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        if (state != null) return;  // recreated while the picker is open; its result still comes here
        if (!startPicker(Intent.ACTION_OPEN_DOCUMENT) && !startPicker(Intent.ACTION_GET_CONTENT)) {
            Toast.makeText(this, "No app on this phone can pick an image", Toast.LENGTH_LONG).show();
            setResult(RESULT_CANCELED);
            finish();
        }
    }

    private boolean startPicker(String action) {
        try {
            startActivityForResult(new Intent(action).addCategory(Intent.CATEGORY_OPENABLE).setType("image/*"),
                    REQUEST_PICK);
            return true;
        } catch (ActivityNotFoundException e) {
            return false;
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != REQUEST_PICK) return;
        Uri picked = resultCode == RESULT_OK && data != null ? data.getData() : null;
        if (picked == null) {
            setResult(RESULT_CANCELED);
            finish();
            return;
        }
        // A document provider may fetch the image over the network: copy off the main thread.
        new Thread(() -> {
            Uri copy = copy(picked);
            runOnUiThread(() -> {
                if (copy != null) {
                    setResult(RESULT_OK, new Intent().setData(copy));
                } else {
                    Toast.makeText(this, "Cannot read the picked image", Toast.LENGTH_LONG).show();
                    setResult(RESULT_CANCELED);
                }
                finish();
            });
        }, "zb-image-pick").start();
    }

    private Uri copy(Uri picked) {
        try {
            return PickedImageProvider.store(this, picked);
        } catch (IOException | RuntimeException e) {
            Log.w(TAG, "cannot copy the picked image " + picked, e);
            return null;
        }
    }
}
