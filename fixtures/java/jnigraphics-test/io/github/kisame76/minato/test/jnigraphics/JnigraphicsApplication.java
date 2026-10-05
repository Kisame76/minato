package io.github.kisame76.minato.test.jnigraphics;

import android.app.Application;
import android.content.Context;
import android.content.res.AssetManager;
import android.graphics.Bitmap;
import android.util.Log;

/**
 * ADR-0041 a5's test: the app's JNI library (jni/jnigraphics_test.c) links the NDK's libjnigraphics, reads two Java
 * bitmaps through AndroidBitmap_*, writes into the first row of one, and decodes the APK's picture.png through
 * AImageDecoder. It logs, with the tag jnigraphics-test, what each call answered, then the first row as Java reads it
 * back, and ends with "done"; tools/android-reference.sh compares it with Android 15's log.
 */
public class JnigraphicsApplication extends Application {
    private static final String TAG = "jnigraphics-test";

    /** The 4 x 4 ARGB_8888 bitmap's colors, unpremultiplied, in rows of full, half, quarter and mixed alpha. */
    private static final int[] COLORS = {
        0xffff0000, 0xff00ff00, 0xff0000ff, 0xff808080,
        0x80ff0000, 0x8000ff00, 0x800000ff, 0x80ffffff,
        0x40ff0000, 0x4000ff00, 0x400000ff, 0x40c86432,
        0x00ff0000, 0x00000000, 0xc80c2238, 0xff4e5a7b,
    };

    /** The 3 x 2 RGB_565 bitmap's colors. */
    private static final int[] OPAQUE = {0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff, 0xff808080, 0xff123456};

    /** What the native side's calls answer, one line each. */
    static native String[] check(Bitmap argb, Bitmap rgb, AssetManager assets);

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        try {
            System.loadLibrary("jnigraphics-test");
            Bitmap argb = Bitmap.createBitmap(4, 4, Bitmap.Config.ARGB_8888);
            argb.setPixels(COLORS, 0, 4, 0, 0, 4, 4);
            Bitmap rgb = Bitmap.createBitmap(3, 2, Bitmap.Config.RGB_565);
            rgb.setPixels(OPAQUE, 0, 3, 0, 0, 3, 2);
            for (String line : check(argb, rgb, base.getAssets())) {
                Log.i(TAG, line);
            }
            Log.i(TAG, String.format("argb: the first row as Java reads it after the native write: %08x %08x %08x %08x",
                    argb.getPixel(0, 0), argb.getPixel(1, 0), argb.getPixel(2, 0), argb.getPixel(3, 0)));
        } catch (Throwable t) {
            Log.i(TAG, "failed: " + t);
            // Its stack, which differs by device, is only shown.
            Log.i(TAG + "-device", Log.getStackTraceString(t));
        }
        Log.i(TAG, "done");
    }
}
