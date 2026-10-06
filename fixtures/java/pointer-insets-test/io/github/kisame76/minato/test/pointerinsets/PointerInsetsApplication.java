package io.github.kisame76.minato.test.pointerinsets;

import android.app.Application;
import android.content.Context;
import android.content.res.Resources;
import android.graphics.Bitmap;
import android.graphics.Insets;
import android.os.Parcel;
import android.util.Log;
import android.view.ContextThemeWrapper;
import android.view.PointerIcon;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsAnimation;
import android.view.animation.DecelerateInterpolator;
import java.util.List;

/**
 * ADR-0042 b's test of gaps 6 and 9's last classes, which Godot's views reach: PointerIcon and WindowInsetsAnimation.
 * Without a window, on Theme.Material.Light, it logs under the tag pointer-insets-test what PointerIcon answers for
 * every system type from 0 to 2003 that has a name, and for the numbers between that have none (its names tell
 * PointerIconType's values, the hidden ones too), for TYPE_CUSTOM, the cache and a null context; for icons made from
 * a bitmap, with both hotspot errors; for its own <pointer-icon> resource, whose hotspot is in dp; for a system icon
 * through a Parcel; and on a View. A custom icon's Parcel holds its Bitmap, which hwui's host flavor cannot parcel
 * on Minato (ADR-0043, the host side's gaps), so the test leaves that out. Then a WindowInsetsAnimation of the IME's
 * insets with and without an interpolator, its Bounds, and a Callback as Godot makes one, also set on a View; then
 * "done". A failing part logs "failed" and its stack under pointer-insets-test-device, which differs by device and
 * is only shown. tools/android-reference.sh takes the log from Android 15 on the M4 plan's display (density 480), and
 * tools/pointer-insets-check.sh compares Minato's with it.
 */
public class PointerInsetsApplication extends Application {
    private static final String TAG = "pointer-insets-test";

    /** One part of the test, which may throw. */
    private interface Part {
        void run() throws Exception;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        Context themed = new ContextThemeWrapper(this, android.R.style.Theme_Material_Light);
        part("system", () -> system(themed));
        part("custom", PointerInsetsApplication::custom);
        part("loaded", () -> loaded(themed.getResources(), themed.getPackageName()));
        part("parcel", () -> parcel(themed));
        part("view", () -> view(themed));
        part("animation", PointerInsetsApplication::animation);
        part("bounds", PointerInsetsApplication::bounds);
        part("callback", () -> callback(themed));
        Log.i(TAG, "done");
    }

    private static void part(String name, Part part) {
        try {
            part.run();
        } catch (Throwable t) {
            Log.i(TAG, name + ": failed: " + t);
            // Its stack, which differs by device, is only shown.
            Log.i(TAG + "-device", Log.getStackTraceString(t));
        }
    }

    /** System icons by type: the named ones, the numbers around them, TYPE_CUSTOM, the cache and a null context. */
    private static void system(Context themed) {
        StringBuilder types = new StringBuilder();
        for (int type : new int[] {0, 1, 2, 999}) {
            types.append(type == 0 ? "" : ", ").append(PointerIcon.getSystemIcon(themed, type));
        }
        Log.i(TAG, "system: " + types);
        for (int first = 1000; first <= 1022; first += 6) {
            types.setLength(0);
            for (int type = first; type < first + 6 && type <= 1023; type++) {
                types.append(type == first ? "" : ", ").append(type).append(' ')
                        .append(name(PointerIcon.getSystemIcon(themed, type)));
            }
            Log.i(TAG, "system: " + types);
        }
        types.setLength(0);
        for (int type = 1999; type <= 2003; type++) {
            types.append(type == 1999 ? "" : ", ").append(type).append(' ')
                    .append(name(PointerIcon.getSystemIcon(themed, type)));
        }
        Log.i(TAG, "system: " + types);
        try {
            Log.i(TAG, "system: -1 gives " + PointerIcon.getSystemIcon(themed, -1));
        } catch (IllegalArgumentException e) {
            Log.i(TAG, "system: -1 throws " + e.getMessage());
        }
        Log.i(TAG, "system: the same hand twice " + (PointerIcon.getSystemIcon(themed, PointerIcon.TYPE_HAND)
                == PointerIcon.getSystemIcon(themed.getApplicationContext(), PointerIcon.TYPE_HAND)));
        try {
            PointerIcon.getSystemIcon(null, PointerIcon.TYPE_ARROW);
            Log.i(TAG, "system: a null context gives an icon");
        } catch (IllegalArgumentException e) {
            Log.i(TAG, "system: a null context throws " + e.getMessage());
        }
    }

    /** Icons made from a bitmap: the hotspot inside, then outside on each axis, and no bitmap. */
    private static void custom() {
        Bitmap bitmap = Bitmap.createBitmap(16, 16, Bitmap.Config.ARGB_8888);
        PointerIcon icon = PointerIcon.create(bitmap, 4f, 6f);
        Log.i(TAG, "custom: " + icon + ", equal to one of the same bitmap and hotspot "
                + icon.equals(PointerIcon.create(bitmap, 4f, 6f)) + ", to one of another hotspot "
                + icon.equals(PointerIcon.create(bitmap, 4f, 7f)));
        float[][] hotspots = {{16f, 0f}, {15.5f, 0f}, {0f, -1f}, {0f, 16f}};
        for (float[] hotspot : hotspots) {
            try {
                Log.i(TAG, "custom: " + hotspot[0] + ", " + hotspot[1] + " gives "
                        + PointerIcon.create(bitmap, hotspot[0], hotspot[1]));
            } catch (IllegalArgumentException e) {
                Log.i(TAG, "custom: " + hotspot[0] + ", " + hotspot[1] + " throws " + e.getMessage());
            }
        }
        try {
            PointerIcon.create(null, 0f, 0f);
        } catch (IllegalArgumentException e) {
            Log.i(TAG, "custom: no bitmap throws " + e.getMessage());
        }
    }

    /** The app's own <pointer-icon>, whose hotspot is in dp, and an XML resource that is none. */
    private static void loaded(Resources resources, String packageName) {
        PointerIcon icon = PointerIcon.load(resources, resources.getIdentifier("pointer", "xml", packageName));
        Log.i(TAG, "loaded: " + icon);
        try {
            PointerIcon.load(resources, resources.getIdentifier("other", "xml", packageName));
            Log.i(TAG, "loaded: <other> gives an icon");
        } catch (IllegalArgumentException e) {
            Log.i(TAG, "loaded: <other> throws " + e.getMessage() + " (" + e.getCause().getMessage() + ")");
        }
        try {
            PointerIcon.load(null, 0);
        } catch (IllegalArgumentException e) {
            Log.i(TAG, "loaded: no resources throws " + e.getMessage());
        }
    }

    /** A system icon through a Parcel: its type alone, read back as the cached icon. */
    private static void parcel(Context themed) {
        PointerIcon text = PointerIcon.getSystemIcon(themed, PointerIcon.TYPE_TEXT);
        Parcel parcel = Parcel.obtain();
        try {
            text.writeToParcel(parcel, 0);
            int size = parcel.dataSize();
            parcel.setDataPosition(0);
            PointerIcon back = PointerIcon.CREATOR.createFromParcel(parcel);
            Log.i(TAG, "parcel: " + text + " in " + size + " bytes, read back as the same icon " + (back == text));
        } finally {
            parcel.recycle();
        }
    }

    /** A View's pointer icon, which a View without a window only keeps. */
    private static void view(Context themed) {
        View view = new View(themed);
        Log.i(TAG, "view: at first " + view.getPointerIcon());
        PointerIcon hand = PointerIcon.getSystemIcon(themed, PointerIcon.TYPE_HAND);
        view.setPointerIcon(hand);
        Log.i(TAG, "view: set to " + view.getPointerIcon() + ", the same " + (view.getPointerIcon() == hand));
    }

    /** A WindowInsetsAnimation of the IME's insets with an interpolator, and one of the status bars without. */
    private static void animation() {
        WindowInsetsAnimation ime = new WindowInsetsAnimation(WindowInsets.Type.ime(), new DecelerateInterpolator(),
                300);
        Log.i(TAG, "animation: type mask " + ime.getTypeMask() + " (ime " + WindowInsets.Type.ime() + "), duration "
                + ime.getDurationMillis() + ", fraction " + ime.getFraction() + ", interpolated "
                + ime.getInterpolatedFraction() + ", alpha " + ime.getAlpha() + ", interpolator "
                + ime.getInterpolator().getClass().getName());
        ime.setFraction(0.5f);
        ime.setAlpha(0.25f);
        Log.i(TAG, "animation: fraction " + ime.getFraction() + ", interpolated " + ime.getInterpolatedFraction()
                + ", alpha " + ime.getAlpha());
        WindowInsetsAnimation bars = new WindowInsetsAnimation(WindowInsets.Type.statusBars(), null, 0);
        bars.setFraction(0.3f);
        Log.i(TAG, "animation: status bars' type mask " + bars.getTypeMask() + ", interpolator "
                + bars.getInterpolator() + ", fraction " + bars.getFraction() + ", interpolated "
                + bars.getInterpolatedFraction());
    }

    private static void bounds() {
        WindowInsetsAnimation.Bounds bounds =
                new WindowInsetsAnimation.Bounds(Insets.of(0, 0, 0, 100), Insets.of(0, 0, 0, 800));
        Log.i(TAG, "bounds: lower " + bounds.getLowerBound() + ", upper " + bounds.getUpperBound());
        Log.i(TAG, "bounds: " + bounds + ", inset by 10, 20, 30, 50 " + bounds.inset(Insets.of(10, 20, 30, 50)));
    }

    /** A Callback as Godot makes one for the IME's insets: its mode and default answers, set on a View. */
    private static void callback(Context themed) {
        WindowInsetsAnimation.Callback callback =
                new WindowInsetsAnimation.Callback(WindowInsetsAnimation.Callback.DISPATCH_MODE_STOP) {
                    @Override
                    public WindowInsets onProgress(WindowInsets insets, List<WindowInsetsAnimation> running) {
                        return insets;
                    }
                };
        WindowInsetsAnimation ime = new WindowInsetsAnimation(WindowInsets.Type.ime(), null, 300);
        WindowInsetsAnimation.Bounds bounds =
                new WindowInsetsAnimation.Bounds(Insets.NONE, Insets.of(0, 0, 0, 800));
        callback.onPrepare(ime);
        boolean same = callback.onStart(ime, bounds) == bounds;
        callback.onEnd(ime);
        Log.i(TAG, "callback: dispatch mode " + callback.getDispatchMode() + ", onStart gives its bounds " + same);
        View view = new View(themed);
        view.setWindowInsetsAnimationCallback(callback);
        Log.i(TAG, "callback: set on a View");
    }

    private static String name(PointerIcon icon) {
        String text = icon.toString();
        return text.substring(text.indexOf('=') + 1, text.indexOf(','));
    }
}
