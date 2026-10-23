package io.github.kisame76.minato.test.photos;

import android.app.Activity;
import android.content.ClipData;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.util.Log;
import java.io.InputStream;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;

/**
 * ADR-0057 step c's test: an app picks photos with the system photo picker (ACTION_PICK_IMAGES) and logs everything it
 * can see of each pick: the grant flags, the picker URI's form (the id masked), the provider's columns, the type, the
 * bytes' SHA-256 and the grants. It first asks for a picker URI it holds no grant for, then picks one photo and two.
 * The photos are red.jpg and blue.jpg (Android: pushed to /sdcard/Pictures and picked by taps; the Mac's harness
 * answers the request with the same files). Logs under the tag photos-test; tools/android-reference.sh takes the log
 * from Android 15 and tools/photos-check.sh compares Minato's with it.
 */
public class PhotosActivity extends Activity {
    static final String TAG = "photos-test";
    static final String PICK_IMAGES = "android.provider.action.PICK_IMAGES";
    static final String EXTRA_PICK_IMAGES_MAX = "android.provider.extra.PICK_IMAGES_MAX";
    private static final String[] NAMES = {"single", "two"};
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mStarted;
    private int mNext;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private static String type(Throwable e) {
        return e.getClass().getName();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus || mStarted) {
            return;
        }
        mStarted = true;
        mHandler.post(() -> {
            ungranted();
            startNext();
        });
    }

    /** A picker URI the app holds no grant for. */
    private void ungranted() {
        ContentResolver resolver = getContentResolver();
        Uri uri = Uri.parse("content://media/picker/0/com.android.providers.media.photopicker/media/1");
        try (Cursor cursor = resolver.query(uri, null, null, null, null)) {
            log("ungranted query: " + (cursor == null ? "null" : "cursor"));
        } catch (RuntimeException e) {
            log("ungranted query: " + type(e));
        }
        try (InputStream in = resolver.openInputStream(uri)) {
            log("ungranted open: " + (in == null ? "null" : "stream"));
        } catch (Exception e) {
            log("ungranted open: " + type(e));
        }
        log("ungranted check: " + checkCallingOrSelfUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION));
    }

    private void startNext() {
        if (mNext == NAMES.length) {
            log("done");
            finish();
            return;
        }
        int index = mNext++;
        Intent intent = new Intent(PICK_IMAGES);
        if (index == 1) {
            intent.putExtra(EXTRA_PICK_IMAGES_MAX, 2);
        }
        log("start " + NAMES[index]);
        startActivityForResult(intent, index);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        log("result " + NAMES[requestCode] + ": code=" + resultCode + " data=" + (data == null ? "null" : "set"));
        if (data != null) {
            int grant = Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                    | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION;
            log("flags: " + (data.getFlags() & grant));
            List<Uri> uris = new ArrayList<>();
            ClipData clip = data.getClipData();
            log("data: " + (data.getData() == null ? "null" : "uri") + " clip: "
                    + (clip == null ? "null" : String.valueOf(clip.getItemCount())));
            if (data.getData() != null) {
                uris.add(data.getData());
            }
            if (clip != null) {
                for (int i = 0; i < clip.getItemCount(); i++) {
                    if (!uris.contains(clip.getItemAt(i).getUri())) {
                        uris.add(clip.getItemAt(i).getUri());
                    }
                }
            }
            for (Uri uri : uris) {
                dump(uri);
            }
        }
        mHandler.post(this::startNext);
    }

    /** The picker URI with its photo's id masked: the form is Android's, the id (MediaStore's) differs from run to run. */
    private static String form(Uri uri) {
        List<String> segments = new ArrayList<>(uri.getPathSegments());
        if (!segments.isEmpty()) {
            segments.set(segments.size() - 1, "<id>");
        }
        return uri.getScheme() + "://" + uri.getAuthority() + "/" + String.join("/", segments);
    }

    /** What the app sees of one picked photo. */
    private void dump(Uri uri) {
        ContentResolver resolver = getContentResolver();
        log("uri: " + form(uri));
        try (Cursor cursor = resolver.query(uri, null, null, null, null)) {
            StringBuilder row = new StringBuilder("row:");
            if (cursor == null || !cursor.moveToFirst()) {
                row.append(" none");
            } else {
                for (int i = 0; i < cursor.getColumnCount(); i++) {
                    String name = cursor.getColumnName(i);
                    // Times, and the ids of the picker, differ from run to run.
                    // Times and ids differ from run to run; the path of the file differs between Android's FUSE
                    // path and Minato's clone (a departure of the provider).
                    String value = name.equals("datetaken") || name.equals("date_modified") || name.equals("_id")
                            ? "<set>" : name.equals("_data") ? "<path>"
                            : String.valueOf(cursor.getString(i)).replaceAll("[0-9]{6,}", "<id>");
                    row.append(i == 0 ? " " : ", ").append(name).append('=').append(value);
                }
            }
            log(row.toString());
        } catch (RuntimeException e) {
            log("row: " + type(e));
        }
        log("type: " + resolver.getType(uri));
        try (InputStream in = resolver.openInputStream(uri)) {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            byte[] buffer = new byte[4096];
            int total = 0;
            for (int n; (n = in.read(buffer)) > 0; total += n) {
                digest.update(buffer, 0, n);
            }
            StringBuilder hex = new StringBuilder();
            for (byte b : digest.digest()) {
                hex.append(String.format("%02x", b));
            }
            log("read: " + total + " " + hex);
        } catch (Exception e) {
            log("read: " + type(e));
        }
        try (ParcelFileDescriptor fd = resolver.openFileDescriptor(uri, "r")) {
            log("fd: " + fd.getStatSize());
        } catch (Exception e) {
            log("fd: " + type(e));
        }
        try (ParcelFileDescriptor fd = resolver.openFileDescriptor(uri, "rw")) {
            log("write fd: opened");
        } catch (Exception e) {
            log("write fd: " + type(e));
        }
        log("check read: " + checkCallingOrSelfUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
                + " write: " + checkCallingOrSelfUriPermission(uri, Intent.FLAG_GRANT_WRITE_URI_PERMISSION));
        try {
            resolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            log("persist: ok " + resolver.getPersistedUriPermissions().size());
            resolver.releasePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            log("released: " + resolver.getPersistedUriPermissions().size());
        } catch (RuntimeException e) {
            log("persist: " + type(e));
        }
    }
}
