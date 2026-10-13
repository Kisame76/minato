package io.github.kisame76.minato.test.f2types;

import android.accessibilityservice.GestureDescription;
import android.accessibilityservice.MagnificationConfig;
import android.app.Activity;
import android.app.AliasActivity;
import android.app.ApplicationErrorReport;
import android.app.Dialog;
import android.app.DialogFragment;
import android.app.DirectAction;
import android.app.LauncherActivity;
import android.app.RecoverableSecurityException;
import android.app.WallpaperColors;
import android.app.WallpaperInfo;
import android.content.DialogInterface;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Path;
import android.inputmethodservice.Keyboard;
import android.media.AudioDescriptor;
import android.media.AudioProfile;
import android.media.ExifInterface;
import android.media.MediaTimestamp;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.Parcel;
import android.os.health.HealthStats;
import android.os.health.PackageHealthStats;
import android.os.health.PidHealthStats;
import android.os.health.ProcessHealthStats;
import android.os.health.ServiceHealthStats;
import android.os.health.TimerStat;
import android.os.health.UidHealthStats;
import android.provider.LiveFolders;
import android.provider.SyncStateContract;
import android.util.Log;
import android.view.View;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.InputBinding;
import android.view.inputmethod.InputConnectionWrapper;
import android.widget.TextView;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;

/**
 * The test of framework breadth batch F2, used as an app does. It runs on the main thread when the launcher activity
 * first has the window focus, part after part with a pause for the windows to settle, and logs under the tag
 * f2-types-test; tools/android-reference.sh takes the log from Android 15, and tools/f2-types-check.sh compares
 * Minato's with it.
 */
public class F2Activity extends Activity {
    static final String TAG = "f2-types-test";
    private static final int SETTLE_MS = 700;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mStarted;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private interface Part {
        void run() throws Exception;
    }

    private static void part(String name, Part part) {
        try {
            part.run();
        } catch (Throwable t) {
            log(name + ": " + t.getClass().getName() + ": " + t.getMessage());
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus || mStarted) {
            return;
        }
        mStarted = true;
        final Runnable[] steps = {
            () -> part("exif", this::exif),
            () -> part("wallpaper", this::wallpaper),
            () -> part("data", this::data),
            () -> part("loads", this::loads),
            () -> part("dialogFragment", this::dialogFragment),
            () -> part("keyboard", this::keyboard),
            () -> {
                log("done");
                finish();
            },
        };
        final int[] index = {0};
        Runnable next = new Runnable() {
            @Override
            public void run() {
                steps[index[0]++].run();
                if (index[0] < steps.length) {
                    mHandler.postDelayed(this, SETTLE_MS);
                }
            }
        };
        mHandler.post(next);
    }

    /** ExifInterface on a JPEG the app makes: attributes written, saved and read back. */
    private void exif() throws Exception {
        Bitmap bitmap = Bitmap.createBitmap(16, 16, Bitmap.Config.ARGB_8888);
        bitmap.eraseColor(Color.rgb(200, 30, 30));
        File file = new File(getCacheDir(), "f2.jpg");
        try (FileOutputStream out = new FileOutputStream(file)) {
            bitmap.compress(Bitmap.CompressFormat.JPEG, 90, out);
        }
        ExifInterface exif = new ExifInterface(file.getAbsolutePath());
        log("exif fresh: make=" + exif.getAttribute(ExifInterface.TAG_MAKE) + " orientation="
                + exif.getAttributeInt(ExifInterface.TAG_ORIENTATION, -1) + " thumbnail=" + exif.hasThumbnail());
        exif.setAttribute(ExifInterface.TAG_MAKE, "Minato");
        exif.setAttribute(ExifInterface.TAG_MODEL, "Test 1");
        exif.setAttribute(ExifInterface.TAG_ORIENTATION, String.valueOf(ExifInterface.ORIENTATION_ROTATE_90));
        exif.setAttribute(ExifInterface.TAG_F_NUMBER, "2.8");
        exif.setAttribute(ExifInterface.TAG_DATETIME, "2024:02:29 12:34:56");
        exif.setAttribute(ExifInterface.TAG_GPS_LATITUDE, "48/1,30/1,0/1");
        exif.setAttribute(ExifInterface.TAG_GPS_LATITUDE_REF, "N");
        exif.setAttribute(ExifInterface.TAG_GPS_LONGITUDE, "12/1,15/1,0/1");
        exif.setAttribute(ExifInterface.TAG_GPS_LONGITUDE_REF, "W");
        exif.setAttribute(ExifInterface.TAG_GPS_ALTITUDE, "120/1");
        exif.setAttribute(ExifInterface.TAG_GPS_ALTITUDE_REF, "0");
        exif.saveAttributes();
        ExifInterface again = new ExifInterface(file.getAbsolutePath());
        float[] latLong = new float[2];
        boolean has = again.getLatLong(latLong);
        log("exif saved: make=" + again.getAttribute(ExifInterface.TAG_MAKE) + " model="
                + again.getAttribute(ExifInterface.TAG_MODEL) + " orientation="
                + again.getAttributeInt(ExifInterface.TAG_ORIENTATION, -1) + " f=" + again.getAttributeDouble(
                ExifInterface.TAG_F_NUMBER, 0) + " date=" + again.getAttribute(ExifInterface.TAG_DATETIME));
        log("exif gps: " + has + " " + latLong[0] + " " + latLong[1] + " alt=" + again.getAltitude(0) + " width="
                + again.getAttributeInt(ExifInterface.TAG_IMAGE_WIDTH, -1));
        log("exif thumbnail: " + again.hasThumbnail() + " " + (again.getThumbnail() == null));
    }

    /** WallpaperColors from a bitmap with two flat halves. */
    private void wallpaper() {
        Bitmap bitmap = Bitmap.createBitmap(40, 40, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(bitmap);
        canvas.drawColor(Color.rgb(20, 60, 200));
        canvas.save();
        canvas.clipRect(0, 0, 40, 10);
        canvas.drawColor(Color.rgb(230, 200, 20));
        canvas.restore();
        WallpaperColors colors = WallpaperColors.fromBitmap(bitmap);
        log("wallpaper: primary=" + Integer.toHexString(colors.getPrimaryColor().toArgb()) + " secondary="
                + (colors.getSecondaryColor() == null ? "none"
                        : Integer.toHexString(colors.getSecondaryColor().toArgb())) + " hints="
                + colors.getColorHints());
        WallpaperColors plain = new WallpaperColors(Color.valueOf(Color.RED), Color.valueOf(Color.GREEN), null);
        log("wallpaper made: " + plain.getPrimaryColor() + " " + plain.getSecondaryColor() + " "
                + plain.getTertiaryColor() + " equals=" + plain.equals(new WallpaperColors(Color.valueOf(Color.RED),
                Color.valueOf(Color.GREEN), null)));
    }

    /** Data types through their builders, constants and parcels. */
    private void data() throws Exception {
        ApplicationErrorReport report = new ApplicationErrorReport();
        report.type = ApplicationErrorReport.TYPE_CRASH;
        report.packageName = getPackageName();
        report.processName = "proc";
        report.time = 1234L;
        report.systemApp = false;
        Parcel parcel = Parcel.obtain();
        report.writeToParcel(parcel, 0);
        parcel.setDataPosition(0);
        ApplicationErrorReport back = ApplicationErrorReport.CREATOR.createFromParcel(parcel);
        parcel.recycle();
        log("report: type=" + back.type + " package=" + back.packageName + " process=" + back.processName
                + " time=" + back.time + " system=" + back.systemApp);
        MediaTimestamp stamp = new MediaTimestamp(5000L, 7000000L, 1.5f);
        log("timestamp: " + stamp.getAnchorMediaTimeUs() + " " + stamp.getAnchorSystemNanoTime() + " "
                + stamp.getMediaClockRate() + " " + MediaTimestamp.TIMESTAMP_UNKNOWN.getMediaClockRate());
        DirectAction direct = new DirectAction.Builder("id1").build();
        log("direct: " + direct.getId() + " " + direct.getLocusId());
        Path path = new Path();
        path.moveTo(10f, 10f);
        path.lineTo(100f, 10f);
        GestureDescription gesture = new GestureDescription.Builder()
                .addStroke(new GestureDescription.StrokeDescription(path, 5L, 200L)).build();
        GestureDescription.StrokeDescription stroke = gesture.getStroke(0);
        log("gesture: " + gesture.getStrokeCount() + " start=" + stroke.getStartTime() + " duration="
                + stroke.getDuration() + " continued=" + stroke.willContinue());
        MagnificationConfig config = new MagnificationConfig.Builder()
                .setMode(MagnificationConfig.MAGNIFICATION_MODE_FULLSCREEN).setScale(2.5f)
                .setCenterX(10f).setCenterY(20f).build();
        log("magnification: mode=" + config.getMode() + " scale=" + config.getScale() + " center="
                + config.getCenterX() + "," + config.getCenterY());
        TimerStat timer = new TimerStat(3, 4500L);
        Parcel timerParcel = Parcel.obtain();
        timer.writeToParcel(timerParcel, 0);
        timerParcel.setDataPosition(0);
        TimerStat timerBack = TimerStat.CREATOR.createFromParcel(timerParcel);
        timerParcel.recycle();
        log("timer: " + timerBack.getCount() + " " + timerBack.getTime());
        log("health keys: " + UidHealthStats.MEASUREMENT_REALTIME_BATTERY_MS + " "
                + UidHealthStats.TIMER_FOREGROUND_ACTIVITY + " " + PidHealthStats.MEASUREMENT_WAKE_NESTING_COUNT
                + " " + ProcessHealthStats.MEASUREMENT_USER_TIME_MS + " " + PackageHealthStats.STATS_SERVICES
                + " " + ServiceHealthStats.MEASUREMENT_START_SERVICE_COUNT + " "
                + HealthStats.class.getSimpleName());
        BaseInputConnection base = new BaseInputConnection(new View(this), false);
        InputConnectionWrapper wrapper = new InputConnectionWrapper(base, true);
        log("wrapper: before=" + wrapper.getTextBeforeCursor(3, 0) + " after=" + wrapper.getTextAfterCursor(3, 0)
                + " commit=" + wrapper.commitText("x", 1));
        InputBinding binding = new InputBinding(base, null, 42, 43);
        log("binding: uid=" + binding.getUid() + " pid=" + binding.getPid() + " connection="
                + (binding.getConnection() == base));
        log("contract: " + SyncStateContract.Constants.ACCOUNT_NAME + " " + SyncStateContract.Constants.DATA + " "
                + LiveFolders.DISPLAY_MODE_LIST + " " + LiveFolders.NAME);
        RecoverableSecurityException ex = new RecoverableSecurityException(new SecurityException("denied"),
                "Needs your OK", new android.app.RemoteAction(android.graphics.drawable.Icon.createWithResource(this,
                android.R.drawable.ic_menu_add), "Allow", "Allow it", android.app.PendingIntent.getActivity(this, 0,
                new android.content.Intent("x"), android.app.PendingIntent.FLAG_IMMUTABLE)));
        log("recoverable: " + ex.getMessage() + " | " + ex.getUserMessage() + " | " + ex.getUserAction().getTitle());
    }

    /** Classes that only have to load, with what they hold as constants or interfaces. */
    private void loads() throws Exception {
        Class<?>[] classes = {AliasActivity.class, LauncherActivity.class, WallpaperInfo.class, AudioProfile.class,
            AudioDescriptor.class, HealthStats.class};
        StringBuilder out = new StringBuilder("loaded:");
        for (Class<?> c : classes) {
            out.append(' ').append(c.getSimpleName()).append(c.isInterface() ? "(i)" : "");
        }
        log(out.toString());
    }

    /** A DialogFragment shown, seen and dismissed. */
    public static class Fragment extends DialogFragment {
        @Override
        public void onCreate(Bundle state) {
            super.onCreate(state);
            log("fragment onCreate");
        }

        @Override
        public Dialog onCreateDialog(Bundle state) {
            log("fragment onCreateDialog");
            Dialog dialog = new Dialog(getActivity());
            TextView text = new TextView(getActivity());
            text.setText("hello");
            dialog.setContentView(text);
            return dialog;
        }

        @Override
        public void onStart() {
            super.onStart();
            log("fragment onStart showing=" + getDialog().isShowing());
        }

        @Override
        public void onDismiss(DialogInterface dialog) {
            super.onDismiss(dialog);
            log("fragment onDismiss");
        }
    }

    private void dialogFragment() {
        Fragment fragment = new Fragment();
        fragment.show(getFragmentManager(), "tag1");
        getFragmentManager().executePendingTransactions();
        log("fragment shown: tag=" + fragment.getTag() + " showsDialog=" + fragment.getShowsDialog() + " cancelable="
                + fragment.isCancelable() + " added=" + fragment.isAdded());
        fragment.dismiss();
        getFragmentManager().executePendingTransactions();
        log("fragment dismissed: added=" + fragment.isAdded());
    }

    /** A Keyboard from an XML file. */
    private void keyboard() {
        Keyboard keyboard = new Keyboard(this, getResources().getIdentifier("qwerty", "xml", getPackageName()));
        log("keyboard: keys=" + keyboard.getKeys().size() + " modifiers=" + keyboard.getModifierKeys().size()
                + " shifted=" + keyboard.isShifted() + " code0=" + keyboard.getKeys().get(0).codes[0] + " label3="
                + keyboard.getKeys().get(3).label + " sticky=" + keyboard.getKeys().get(3).sticky + " repeat="
                + keyboard.getKeys().get(4).repeatable + " codes4=" + keyboard.getKeys().get(4).codes.length);
    }
}
