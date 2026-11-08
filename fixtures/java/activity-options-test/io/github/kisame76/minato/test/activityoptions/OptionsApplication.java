package io.github.kisame76.minato.test.activityoptions;

import android.app.Activity;
import android.app.ActivityOptions;
import android.app.Application;
import android.graphics.Bitmap;
import android.graphics.Rect;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;

import java.util.Arrays;
import java.util.TreeSet;
import java.util.concurrent.Callable;

/**
 * The options objects apps pass to startActivity: each factory and setter of ActivityOptions, logged with the keys and
 * scalar values of its bundle (the others by type name) and the getters' answers, or with the class of the exception.
 * Window transitions play no part: nothing here starts an activity.
 */
public class OptionsApplication extends Application {
    private static final String TAG = "activity-options-test";

    private static String show(Object value) {
        if (value == null) return "null";
        if (value instanceof Integer || value instanceof Boolean || value instanceof Long || value instanceof String) {
            return String.valueOf(value);
        }
        if (value instanceof Rect) return ((Rect) value).toShortString();
        if (value instanceof Bundle) return dump((Bundle) value);
        if (value instanceof int[]) return Arrays.toString((int[]) value);
        return value.getClass().getName();
    }

    private static String dump(Bundle bundle) {
        if (bundle == null) return "null bundle";
        final StringBuilder out = new StringBuilder("{");
        for (final String key : new TreeSet<>(bundle.keySet())) {
            out.append(out.length() > 1 ? ", " : "").append(key).append('=').append(show(bundle.get(key)));
        }
        return out.append('}').toString();
    }

    private static void log(String name, Callable<Object> call) {
        log(TAG, name, call);
    }

    private static void log(String tag, String name, Callable<Object> call) {
        String result;
        try {
            final Object value = call.call();
            result = value instanceof Bundle ? dump((Bundle) value) : show(value);
        } catch (Throwable e) {
            result = "throws " + e.getClass().getName();
        }
        Log.i(tag, name + ": " + result);
    }

    @Override
    public void onCreate() {
        super.onCreate();
        new Handler(Looper.getMainLooper()).post(this::run);
    }

    private void run() {
        final View view = new View(this);
        final Bitmap bitmap = Bitmap.createBitmap(4, 4, Bitmap.Config.ARGB_8888);
        log("makeBasic().toBundle()", () -> ActivityOptions.makeBasic().toBundle());
        log("makeCustomAnimation(1, 2).toBundle()", () -> ActivityOptions.makeCustomAnimation(this, 1, 2).toBundle());
        log("makeCustomAnimation(1, 2, 3).toBundle()", () -> ActivityOptions.makeCustomAnimation(this, 1, 2, 3)
                .toBundle());
        log("makeScaleUpAnimation(5, 6, 7, 8).toBundle()", () -> ActivityOptions.makeScaleUpAnimation(view, 5, 6, 7, 8)
                .toBundle());
        log("makeClipRevealAnimation(5, 6, 7, 8).toBundle()", () -> ActivityOptions.makeClipRevealAnimation(view, 5, 6,
                7, 8).toBundle());
        // The thumbnail travels as a HardwareBuffer of a hardware copy of the bitmap, which Minato's bitmaps have no
        // buffer for yet: the value of android:activity.animThumbnail differs, so the line is only shown.
        log(TAG + "-device", "makeThumbnailScaleUpAnimation(5, 6).toBundle()",
                () -> ActivityOptions.makeThumbnailScaleUpAnimation(view, bitmap, 5, 6).toBundle());
        log("makeTaskLaunchBehind().toBundle()", () -> ActivityOptions.makeTaskLaunchBehind().toBundle());
        log("makeScaleUpAnimation(null view)", () -> ActivityOptions.makeScaleUpAnimation(null, 1, 2, 3, 4));
        log("makeThumbnailScaleUpAnimation(null bitmap)", () -> ActivityOptions.makeThumbnailScaleUpAnimation(view,
                null, 1, 2));

        log("getLaunchBounds() of makeBasic", () -> ActivityOptions.makeBasic().getLaunchBounds());
        log("setLaunchBounds(1,2,30,40).getLaunchBounds()", () -> ActivityOptions.makeBasic()
                .setLaunchBounds(new Rect(1, 2, 30, 40)).getLaunchBounds());
        log("setLaunchBounds returns the same object", () -> {
            final ActivityOptions o = ActivityOptions.makeBasic();
            return o.setLaunchBounds(new Rect(1, 2, 3, 4)) == o;
        });
        log("setLaunchBounds copies its argument", () -> {
            final Rect r = new Rect(1, 2, 30, 40);
            final ActivityOptions o = ActivityOptions.makeBasic().setLaunchBounds(r);
            r.set(9, 9, 9, 9);
            return o.getLaunchBounds();
        });
        log("setLaunchBounds(1,2,30,40).toBundle()", () -> ActivityOptions.makeBasic()
                .setLaunchBounds(new Rect(1, 2, 30, 40)).toBundle());
        log("setLaunchBounds(null).getLaunchBounds()", () -> ActivityOptions.makeBasic().setLaunchBounds(null)
                .getLaunchBounds());
        log("setLaunchDisplayId(3)", () -> {
            final ActivityOptions o = ActivityOptions.makeBasic().setLaunchDisplayId(3);
            return o.getLaunchDisplayId() + " " + dump(o.toBundle());
        });
        log("getLaunchDisplayId() of makeBasic", () -> ActivityOptions.makeBasic().getLaunchDisplayId());
        log("setLockTaskEnabled(true)", () -> {
            final ActivityOptions o = ActivityOptions.makeBasic().setLockTaskEnabled(true);
            return o.getLockTaskMode() + " " + dump(o.toBundle());
        });
        log("setShareIdentityEnabled(true)", () -> {
            final ActivityOptions o = ActivityOptions.makeBasic().setShareIdentityEnabled(true);
            return o.isShareIdentityEnabled() + " " + dump(o.toBundle());
        });
        log("isShareIdentityEnabled() of makeBasic", () -> ActivityOptions.makeBasic().isShareIdentityEnabled());
        log("setSplashScreenStyle(1)", () -> {
            final ActivityOptions o = ActivityOptions.makeBasic().setSplashScreenStyle(1);
            return o.getSplashScreenStyle() + " " + dump(o.toBundle());
        });
        log("getSplashScreenStyle() of makeBasic", () -> ActivityOptions.makeBasic().getSplashScreenStyle());
        log("setAppVerificationBundle(x=1).toBundle()", () -> {
            final Bundle b = new Bundle();
            b.putInt("x", 1);
            return ActivityOptions.makeBasic().setAppVerificationBundle(b).toBundle();
        });
        log("setPendingIntentBackgroundActivityStartMode(1)", () -> {
            final ActivityOptions o = ActivityOptions.makeBasic().setPendingIntentBackgroundActivityStartMode(1);
            return o.getPendingIntentBackgroundActivityStartMode() + " " + dump(o.toBundle());
        });
        log("setPendingIntentCreatorBackgroundActivityStartMode(2)", () -> {
            final ActivityOptions o = ActivityOptions.makeBasic()
                    .setPendingIntentCreatorBackgroundActivityStartMode(2);
            return o.getPendingIntentCreatorBackgroundActivityStartMode() + " " + dump(o.toBundle());
        });
        log("requestUsageTimeReport(null)", () -> {
            ActivityOptions.makeBasic().requestUsageTimeReport(null);
            return "returned";
        });
        log("update(makeCustomAnimation) into makeBasic", () -> {
            final ActivityOptions o = ActivityOptions.makeBasic().setLaunchBounds(new Rect(1, 2, 3, 4));
            o.update(ActivityOptions.makeCustomAnimation(this, 1, 2));
            return dump(o.toBundle()) + " " + o.getLaunchBounds();
        });
        log("update(makeBasic) into makeCustomAnimation", () -> {
            final ActivityOptions o = ActivityOptions.makeCustomAnimation(this, 1, 2);
            o.update(ActivityOptions.makeBasic());
            return dump(o.toBundle());
        });
        log("update(makeScaleUpAnimation) into makeCustomAnimation", () -> {
            final ActivityOptions o = ActivityOptions.makeCustomAnimation(this, 1, 2);
            o.update(ActivityOptions.makeScaleUpAnimation(view, 5, 6, 7, 8));
            return dump(o.toBundle());
        });
        log("update(null)", () -> {
            ActivityOptions.makeBasic().update(null);
            return "returned";
        });
        log("toBundle() twice is equal", () -> {
            final ActivityOptions o = ActivityOptions.makeCustomAnimation(this, 1, 2);
            return dump(o.toBundle()).equals(dump(o.toBundle()));
        });
        log("setPendingIntentBackgroundActivityStartMode(7)", () -> {
            final ActivityOptions o = ActivityOptions.makeBasic().setPendingIntentBackgroundActivityStartMode(7);
            return o.getPendingIntentBackgroundActivityStartMode() + " " + dump(o.toBundle());
        });
        log("setPendingIntentBackgroundActivityLaunchAllowed(false)", () -> {
            final ActivityOptions o = ActivityOptions.makeBasic();
            o.setPendingIntentBackgroundActivityLaunchAllowed(false);
            return o.isPendingIntentBackgroundActivityLaunchAllowed() + " "
                    + o.getPendingIntentBackgroundActivityStartMode() + " " + dump(o.toBundle());
        });
        log("isPendingIntentBackgroundActivityLaunchAllowed() of makeBasic", () -> ActivityOptions.makeBasic()
                .isPendingIntentBackgroundActivityLaunchAllowed());
        log("setSplashScreenStyle(5) is ignored", () -> ActivityOptions.makeBasic().setSplashScreenStyle(5)
                .getSplashScreenStyle());
        log("makeLaunchIntoPip().toBundle()", () -> ActivityOptions.makeLaunchIntoPip(
                new android.app.PictureInPictureParams.Builder().build()).toBundle());
        // The bundle constructor and fromBundle are hidden: only Minato's lines are compared by eye.
        log(TAG + "-device", "fromBundle(custom animation bundle).toBundle() equals the bundle", () -> {
            final Bundle b = ActivityOptions.makeCustomAnimation(this, 1, 2, 3).setLaunchDisplayId(4).toBundle();
            final Object o = Class.forName("android.app.ActivityOptions").getMethod("fromBundle", Bundle.class)
                    .invoke(null, b);
            return dump(((ActivityOptions) o).toBundle()).equals(dump(b));
        });
        log(TAG + "-device", "fromBundle(scale up bundle).toBundle() equals the bundle", () -> {
            final Bundle b = ActivityOptions.makeScaleUpAnimation(view, 5, 6, 7, 8).setLockTaskEnabled(true)
                    .toBundle();
            final Object o = Class.forName("android.app.ActivityOptions").getMethod("fromBundle", Bundle.class)
                    .invoke(null, b);
            return dump(((ActivityOptions) o).toBundle()).equals(dump(b));
        });
        log("EXTRA_USAGE_TIME_REPORT", () -> ActivityOptions.EXTRA_USAGE_TIME_REPORT);
        log("MODE_BACKGROUND_ACTIVITY_START_DENIED", () -> ActivityOptions.MODE_BACKGROUND_ACTIVITY_START_DENIED);
        Log.i(TAG, "done");
    }
}
