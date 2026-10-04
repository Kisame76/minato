package io.github.kisame76.minato.test.context;

import android.Manifest;
import android.app.Application;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.ApplicationInfo;
import android.content.res.Resources;
import android.os.Environment;
import android.os.Process;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.TreeSet;

/**
 * ADR-0039 step c1's test: what an app's Context gives in Application.attachBaseContext, which Android calls as it
 * makes the application, before onCreate. It logs, with the tag context-test, the Context's paths (decision 6) and
 * what each call leaves on disk, a file of its own, SharedPreferences, a string, an asset and permission checks, and
 * ends with "done"; tools/android-reference.sh compares it with Android 15's log. The app starts twice: the second
 * start reads what the first one wrote.
 *
 * The code path's random folders differ by install (decision 6), so they are masked after their shape is checked.
 * The modes are those of what ContextImpl makes itself; the data folder's own comes from the installer.
 */
public class ContextApplication extends Application {
    private static final String TAG = "context-test";
    private static final String PACKAGE = "io.github.kisame76.minato.test.context";

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        try {
            report(base);
        } catch (Throwable t) {
            Log.i(TAG, "failed: " + t);
            // Its stack, which differs by device, is only shown.
            Log.i(TAG + "-device", Log.getStackTraceString(t));
        }
        Log.i(TAG, "done");
    }

    static void report(Context base) throws IOException, ErrnoException {
        Log.i(TAG, "context " + base.getClass().getName());
        Log.i(TAG, "packageName " + base.getPackageName());
        Log.i(TAG, "opPackageName " + base.getOpPackageName());
        // The application is not the Context's yet: Android sets it after attachBaseContext returns.
        Log.i(TAG, "applicationContext " + base.getApplicationContext());

        directory("filesDir", base.getFilesDir(), true);
        directory("cacheDir", base.getCacheDir(), false);
        directory("codeCacheDir", base.getCodeCacheDir(), false);
        directory("noBackupFilesDir", base.getNoBackupFilesDir(), true);
        directory("dir custom", base.getDir("custom", Context.MODE_PRIVATE), true);
        File database = base.getDatabasePath("context-test.db");
        Log.i(TAG, "databasePath " + database + " exists " + database.exists());
        directory("databasePath's folder", database.getParentFile(), true);
        Log.i(TAG, "fileStreamPath " + base.getFileStreamPath("note.txt"));
        directory("externalFilesDir", base.getExternalFilesDir(null), false);
        directory("externalFilesDir Music", base.getExternalFilesDir(Environment.DIRECTORY_MUSIC), false);
        directory("externalCacheDir", base.getExternalCacheDir(), false);
        directory("obbDir", base.getObbDir(), false);
        Log.i(TAG, "externalFilesDirs " + Arrays.toString(base.getExternalFilesDirs(null)));
        Log.i(TAG, "dataDir " + base.getDataDir());
        Log.i(TAG, "packageCodePath " + codePath(base.getPackageCodePath(), "/base.apk"));
        Log.i(TAG, "packageResourcePath " + codePath(base.getPackageResourcePath(), "/base.apk"));
        ApplicationInfo info = base.getApplicationInfo();
        Log.i(TAG, "applicationInfo dataDir " + info.dataDir + " deviceProtectedDataDir " + info.deviceProtectedDataDir);
        Log.i(TAG, "classLoader " + base.getClassLoader().getClass().getName());
        Log.i(TAG, "packageManager " + base.getPackageManager().getClass().getName());

        SharedPreferences prefs = base.getSharedPreferences("context-test", Context.MODE_PRIVATE);
        Log.i(TAG, "prefs " + prefs.getClass().getName() + ", the same again "
                + (prefs == base.getSharedPreferences("context-test", Context.MODE_PRIVATE)));
        Log.i(TAG, "prefs at start " + sorted(prefs.getAll()));
        int start = prefs.getInt("start", 0) + 1;
        SharedPreferences.OnSharedPreferenceChangeListener listener =
                (changed, key) -> Log.i(TAG, "prefs changed " + key);
        prefs.registerOnSharedPreferenceChangeListener(listener);
        prefs.edit().putString("applied", "start " + start).apply();
        boolean committed = prefs.edit().putInt("start", start).putLong("long", 1L << 40).putFloat("float", 1.5f)
                .putBoolean("even", start % 2 == 0).putStringSet("set", new TreeSet<>(Arrays.asList("b", "a")))
                .remove("missing").commit();
        prefs.unregisterOnSharedPreferenceChangeListener(listener);
        Log.i(TAG, "prefs commit " + committed + ", now " + sorted(prefs.getAll()));
        File prefsFile = new File(base.getDataDir(), "shared_prefs/context-test.xml");
        directory("prefs folder", prefsFile.getParentFile(), true);
        Log.i(TAG, "prefs file mode " + mode(prefsFile));
        for (String line : read(new FileInputStream(prefsFile)).split("\n")) {
            Log.i(TAG, "prefs xml " + line);
        }

        try (OutputStream out = base.openFileOutput("note.txt", Context.MODE_PRIVATE)) {
            out.write(("start " + start).getBytes(StandardCharsets.UTF_8));
        }
        Log.i(TAG, "fileList " + new TreeSet<>(Arrays.asList(base.fileList())));
        Log.i(TAG, "note " + read(base.openFileInput("note.txt")) + ", mode "
                + mode(base.getFileStreamPath("note.txt")));

        Resources res = base.getResources();
        Log.i(TAG, "string " + res.getString(res.getIdentifier("greeting", "string", PACKAGE)));
        Log.i(TAG, "asset " + read(base.getAssets().open("context-test.txt")).trim());

        Log.i(TAG, "checkSelfPermission INTERNET " + base.checkSelfPermission(Manifest.permission.INTERNET));
        Log.i(TAG, "checkSelfPermission CAMERA " + base.checkSelfPermission(Manifest.permission.CAMERA));
        Log.i(TAG, "checkPermission INTERNET " + base.checkPermission(Manifest.permission.INTERNET, Process.myPid(),
                Process.myUid()));
        Log.i(TAG, "checkCallingOrSelfPermission CAMERA "
                + base.checkCallingOrSelfPermission(Manifest.permission.CAMERA));

        Log.i(TAG, "dataDir list " + new TreeSet<>(Arrays.asList(base.getDataDir().list())));
    }

    /** A folder a Context call gave: its path, whether it exists, and with `ours` the mode ContextImpl gave it. */
    private static void directory(String name, File folder, boolean ours) throws ErrnoException {
        String line = name + " " + folder + " exists " + folder.isDirectory();
        Log.i(TAG, ours && folder.exists() ? line + ", mode " + mode(folder) : line);
    }

    private static String mode(File file) throws ErrnoException {
        return Integer.toOctalString(Os.stat(file.getPath()).st_mode & 07777);
    }

    private static String read(InputStream in) throws IOException {
        try (InputStream stream = in) {
            return new String(stream.readAllBytes(), StandardCharsets.UTF_8);
        }
    }

    /** The preferences in key order, a set's strings sorted. */
    private static String sorted(Map<String, ?> all) {
        TreeMap<String, Object> sorted = new TreeMap<>();
        for (Map.Entry<String, ?> entry : all.entrySet()) {
            Object value = entry.getValue();
            sorted.put(entry.getKey(), value instanceof Set ? new TreeSet<>((Set<?>) value) : value);
        }
        return sorted.toString();
    }

    /**
     * Decision 6's form, Android's: /data/app/~~<a>/<package>-<b><end>, where <a> and <b> are 16 bytes in URL-safe
     * Base64 with padding. Another shape is logged as it is, so the comparison fails.
     */
    private static String codePath(String path, String end) {
        String random = "[A-Za-z0-9_-]{22}==";
        if (path.matches("/data/app/~~" + random + "/" + PACKAGE.replace(".", "\\.") + "-" + random
                + end.replace(".", "\\."))) {
            return "/data/app/~~<a>/" + PACKAGE + "-<b>" + end;
        }
        return path;
    }
}
