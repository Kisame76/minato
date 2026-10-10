package io.github.kisame76.minato.test.storage;

import android.app.Application;
import android.content.pm.ApplicationInfo;
import android.os.Environment;
import android.util.Log;

import java.io.File;

/**
 * The folders an app asks for before it writes anything (the Unity test APK probe: Unity's IL2CPP builds "/il2cpp/..." from an
 * empty folder name): the internal and external files and cache folders, the code cache, the OBB and media folders and
 * Environment's external storage, each as the path, whether it exists and whether it is a directory.
 */
public class StorageApplication extends Application {
    static final String TAG = "storage-test";

    private static String show(File file) {
        if (file == null) {
            return "null";
        }
        return file.getPath() + (file.isDirectory() ? " (directory)" : file.exists() ? " (file)" : " (absent)");
    }

    private static String text(String path) {
        try (java.io.FileInputStream in = new java.io.FileInputStream(path)) {
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            byte[] buffer = new byte[4096];
            for (int n; (n = in.read(buffer)) > 0; ) {
                out.write(buffer, 0, n);
            }
            return out.toString("UTF-8");
        } catch (java.io.IOException e) {
            return "(" + e.getClass().getSimpleName() + ")";
        }
    }

    private static String link(String path) {
        try {
            return android.system.Os.readlink(path);
        } catch (android.system.ErrnoException e) {
            return "(" + e.getMessage() + ")";
        }
    }

    /** A path with the install's random folder names left out. */
    private static String anon(String path) {
        return path == null ? "null" : path.replaceAll("~~[^/]*/", "~~X/").replaceAll("(?<=[a-z0-9])-[^/]*==/", "-Y/");
    }

    private static String show(File[] files) {
        if (files == null) {
            return "null";
        }
        StringBuilder out = new StringBuilder("[");
        for (int i = 0; i < files.length; i++) {
            out.append(i == 0 ? "" : ", ").append(show(files[i]));
        }
        return out.append("]").toString();
    }

    @Override
    public void onCreate() {
        super.onCreate();
        Log.i(TAG, "files dir: " + show(getFilesDir()));
        Log.i(TAG, "cache dir: " + show(getCacheDir()));
        Log.i(TAG, "code cache dir: " + show(getCodeCacheDir()));
        Log.i(TAG, "no backup files dir: " + show(getNoBackupFilesDir()));
        Log.i(TAG, "data dir: " + show(getDataDir()));
        Log.i(TAG, "external files dir: " + show(getExternalFilesDir(null)));
        Log.i(TAG, "external files dir, pictures: " + show(getExternalFilesDir(Environment.DIRECTORY_PICTURES)));
        Log.i(TAG, "external files dirs: " + show(getExternalFilesDirs(null)));
        Log.i(TAG, "external cache dir: " + show(getExternalCacheDir()));
        Log.i(TAG, "external cache dirs: " + show(getExternalCacheDirs()));
        Log.i(TAG, "obb dir: " + show(getObbDir()));
        Log.i(TAG, "obb dirs: " + show(getObbDirs()));
        Log.i(TAG, "external storage state: " + Environment.getExternalStorageState());
        Log.i(TAG, "external storage directory: " + Environment.getExternalStorageDirectory().getPath());
        Log.i(TAG, "external storage emulated: " + Environment.isExternalStorageEmulated()
                + ", removable " + Environment.isExternalStorageRemovable());
        Log.i(TAG, "data directory: " + show(Environment.getDataDirectory()));
        Log.i(TAG, "download cache directory: " + Environment.getDownloadCacheDirectory().getPath());
        Log.i(TAG, "root directory: " + show(Environment.getRootDirectory()));
        Log.i(TAG, "package code path: " + anon(getPackageCodePath()));
        Log.i(TAG, "package resource path: " + anon(getPackageResourcePath()));
        ApplicationInfo info = getApplicationInfo();
        Log.i(TAG, "application info: data " + info.dataDir + ", device protected " + info.deviceProtectedDataDir
                + ", credential protected " + info.credentialProtectedDataDir + ", source " + anon(info.sourceDir)
                + ", public source " + anon(info.publicSourceDir) + ", native libraries " + anon(info.nativeLibraryDir)
                + ", process " + info.processName + ", uid app " + (info.uid >= 10000));
        Log.i(TAG, "application info 2: native libraries " + anon(info.nativeLibraryDir) + ", root "
                + anon(info.nativeLibraryRootDir) + ", secondary abi "
                + info.secondaryCpuAbi + ", secondary dir " + anon(info.secondaryNativeLibraryDir) + ", splits "
                + (info.splitSourceDirs == null ? "null" : info.splitSourceDirs.length) + ", shared libraries "
                + (info.sharedLibraryFiles == null ? "null" : info.sharedLibraryFiles.length) + ", target "
                + info.targetSdkVersion + ", min " + info.minSdkVersion + ", class " + info.className + ", name "
                + info.name + ", package " + info.packageName + ", enabled " + info.enabled + ", flags "
                + Integer.toHexString(info.flags & (ApplicationInfo.FLAG_HAS_CODE | ApplicationInfo.FLAG_DEBUGGABLE
                        | ApplicationInfo.FLAG_EXTRACT_NATIVE_LIBS | ApplicationInfo.FLAG_SYSTEM)));
        Log.i(TAG, "properties: tmpdir " + System.getProperty("java.io.tmpdir") + ", user.dir "
                + System.getProperty("user.dir") + ", user.home " + System.getProperty("user.home")
                + ", os.name " + System.getProperty("os.name")
                + ", java.vm.name " + System.getProperty("java.vm.name"));
        Log.i(TAG, "environment: TMPDIR " + System.getenv("TMPDIR") + ", ANDROID_DATA " + System.getenv("ANDROID_DATA")
                + ", ANDROID_ROOT " + System.getenv("ANDROID_ROOT") + ", EXTERNAL_STORAGE "
                + System.getenv("EXTERNAL_STORAGE") + ", ANDROID_STORAGE " + System.getenv("ANDROID_STORAGE")
                + ", HOME " + System.getenv("HOME") + ", PATH " + System.getenv("PATH"));
        Log.i(TAG, "working directory: " + new File(".").getAbsoluteFile().getParent());
        for (java.util.Map.Entry<String, String> entry : new java.util.TreeMap<>(System.getenv()).entrySet()) {
            String value = entry.getValue();
            if (value.length() > 60) {
                value = value.substring(0, 60) + "...";
            }
            if (!entry.getKey().startsWith("ANDROID_SOCKET")) {
                Log.i(TAG, "env " + entry.getKey() + "=" + value);
            }
        }
        Log.i(TAG, "done");
    }
}
