package com.zettabridge.launcher;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.webkit.MimeTypeMap;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.InputStream;

/**
 * Images picked through ImagePickActivity, as private copies in the launcher's cache. query
 * answers the columns old apps read, "_data" being the copy's path, which they open directly;
 * openFile serves the copy to apps that read the URI instead. Not exported.
 */
public final class PickedImageProvider extends ContentProvider {
    private static final String DIR = "picked-images";
    /** MediaStore.MediaColumns.DATA: deprecated, and exactly what those apps ask for. */
    private static final String DATA = "_data";
    private static final String MIME_TYPE = "mime_type";

    /**
     * Copies a picked image here and returns its URI. Only the latest pick is kept: by the next
     * one, the app has read the previous copy (Minecraft PE stores its own copy of a skin).
     */
    static Uri store(Context c, Uri picked) throws IOException {
        String type = c.getContentResolver().getType(picked);
        String extension = type != null ? MimeTypeMap.getSingleton().getExtensionFromMimeType(type) : null;
        File dir = new File(c.getCacheDir(), DIR);
        PluginFiles.deleteRecursive(dir);
        File copy = new File(dir, "image-" + System.currentTimeMillis() + "." + (extension != null ? extension : "png"));
        try (InputStream in = c.getContentResolver().openInputStream(picked)) {
            if (in == null) throw new IOException("no data behind " + picked);
            PluginFiles.copyAtomic(in, copy);
        }
        return new Uri.Builder().scheme("content").authority(c.getPackageName() + ".pickedimage")
                .appendPath(copy.getName()).build();
    }

    /** The copy a URI names, or null: only a plain file name directly inside DIR. */
    private File file(Uri uri) {
        String name = uri.getLastPathSegment();
        if (uri.getPathSegments().size() != 1 || name.startsWith(".") || name.contains("/")) return null;
        File file = new File(new File(getContext().getCacheDir(), DIR), name);
        return file.isFile() ? file : null;
    }

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        File file = file(uri);
        if (file == null) return null;
        String[] columns = projection != null ? projection
                : new String[] {DATA, OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE, MIME_TYPE};
        Object[] row = new Object[columns.length];
        for (int i = 0; i < columns.length; i++) {
            switch (columns[i]) {
                case DATA:
                    row[i] = file.getPath();
                    break;
                case OpenableColumns.DISPLAY_NAME:
                    row[i] = file.getName();
                    break;
                case OpenableColumns.SIZE:
                    row[i] = file.length();
                    break;
                case MIME_TYPE:
                    row[i] = getType(uri);
                    break;
                default:
                    break;  // unknown columns read as null
            }
        }
        MatrixCursor cursor = new MatrixCursor(columns, 1);
        cursor.addRow(row);
        return cursor;
    }

    @Override
    public String getType(Uri uri) {
        String name = uri.getLastPathSegment();
        int dot = name != null ? name.lastIndexOf('.') : -1;
        return dot >= 0 ? MimeTypeMap.getSingleton().getMimeTypeFromExtension(name.substring(dot + 1)) : null;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        File file = file(uri);
        if (file == null) throw new FileNotFoundException(uri.toString());
        return ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        return null;
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        return 0;
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) {
        return 0;
    }
}
