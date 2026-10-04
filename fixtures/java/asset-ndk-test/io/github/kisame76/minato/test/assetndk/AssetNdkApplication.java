package io.github.kisame76.minato.test.assetndk;

import android.app.Application;
import android.content.Context;
import android.content.res.AssetManager;
import android.util.Log;

/**
 * ADR-0041 decision 7's test: the app's JNI library (jni/asset_ndk_test.c) takes the Context's AssetManager through
 * AAssetManager_fromJava in Application.attachBaseContext, as Godot takes its project's, and reads the APK's assets
 * through libandroid's AAsset functions. It logs, with the tag asset-ndk-test, what each call answered and ends with
 * "done"; tools/android-reference.sh compares it with Android 15's log.
 */
public class AssetNdkApplication extends Application {
    private static final String TAG = "asset-ndk-test";

    /** What the native side's calls answer for `assets`, one line each; `again` is the same object, got again. */
    static native String[] check(AssetManager assets, AssetManager again);

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        try {
            System.loadLibrary("asset-ndk-test");
            for (String line : check(base.getAssets(), base.getResources().getAssets())) {
                Log.i(TAG, line);
            }
        } catch (Throwable t) {
            Log.i(TAG, "failed: " + t);
            // Its stack, which differs by device, is only shown.
            Log.i(TAG + "-device", Log.getStackTraceString(t));
        }
        Log.i(TAG, "done");
    }
}
