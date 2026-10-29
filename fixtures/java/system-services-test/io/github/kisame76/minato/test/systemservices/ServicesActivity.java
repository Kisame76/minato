package io.github.kisame76.minato.test.systemservices;

import android.app.Activity;
import android.app.UiModeManager;
import android.content.Context;
import android.content.res.Configuration;
import android.media.metrics.LogSessionId;
import android.media.metrics.MediaMetricsManager;
import android.media.metrics.PlaybackSession;
import android.media.metrics.PlaybackStateEvent;
import android.os.Bundle;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.util.Log;
import android.view.HapticFeedbackConstants;
import android.view.View;
import android.view.accessibility.CaptioningManager;

/**
 * Gap 1's batch 3 (the compatibility plan): system services that answered null in Minato, each asked as the libraries
 * that call it ask, once the window has focus, so that its views are attached:
 * <ul>
 *   <li>the vibrator, by name and by class, and the vibrator manager: their classes, the same object by name and by
 *       class, a one-shot vibration and its cancel with the VIBRATE permission, and a view's haptic feedback for a long
 *       press, which View sends to the vibrator itself (use_vibrator_haptic_feedback). Whether the device has a
 *       vibrator, its amplitude control, its IDs and the haptic feedback's answer differ by device and are shown on
 *       the device tag.
 *   <li>the UI mode manager, from the application's context as media3's Util.isTv and AppCompat's night mode ask it:
 *       its class, the same object by class, the current mode type, the night mode as the configuration has it, and
 *       the contrast Material's ColorContrast reads. The night mode itself follows the device's dark theme and is shown
 *       on the device tag.
 *   <li>the captioning manager as media3's TrackSelectionParameters and SubtitleView ask it: its class, the same
 *       object by class, whether captions are on, their locale, font scale and the user's caption style;
 *   <li>the media metrics manager as media3's MediaMetricsListener asks it: its class, the same object by class, a
 *       playback session, a state event reported to it and its close. The session's ID, random, is on the device tag.
 * </ul>
 * A missing class shows as a NoClassDefFoundError line where media3 would meet it, the cast of getSystemService's
 * answer, so that the log tells whether media3 gets past it.
 * Its twin system-services-test-unpermitted, the same sources without the VIBRATE permission, shows the vibrator
 * service's checks: the vibration and the cancel throw SecurityException.
 * Logs under the app's label (system-services-test or its twin's) and, what differs by device, the label with
 * "-device"; "done" at the end.
 */
public class ServicesActivity extends Activity {
    private String mTag;
    private String mDevice;
    private boolean mStarted;
    private View mView;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mTag = getApplicationInfo().loadLabel(getPackageManager()).toString();
        mDevice = mTag + "-device";
        mView = new View(this);
        mView.setHapticFeedbackEnabled(true);
        setContentView(mView);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus || mStarted) {
            return;
        }
        mStarted = true;
        Log.i(mTag, "start");
        vibrator();
        haptics();
        uiMode();
        try {
            captioning();
        } catch (NoClassDefFoundError e) {
            Log.i(mTag, "captioning: " + e.getClass().getName());
        }
        try {
            mediaMetrics();
        } catch (NoClassDefFoundError e) {
            Log.i(mTag, "media metrics: " + e.getClass().getName());
        }
        Log.i(mTag, "done");
    }

    private static String name(Object service) {
        return service == null ? "null" : service.getClass().getName();
    }

    private void vibrator() {
        final Object byName = getSystemService(Context.VIBRATOR_SERVICE);
        final Vibrator vibrator = getSystemService(Vibrator.class);
        Log.i(mTag, "vibrator: " + name(byName) + ", the same by class " + (byName == vibrator));
        final VibratorManager manager = getSystemService(VibratorManager.class);
        Log.i(mTag, "vibrator manager: " + name(manager) + ", the same by name "
                + (manager == getSystemService(Context.VIBRATOR_MANAGER_SERVICE)));
        if (vibrator == null || manager == null) {
            return;
        }
        Log.i(mTag, "default vibrator: " + name(manager.getDefaultVibrator()));
        Log.i(mDevice, "has vibrator " + vibrator.hasVibrator() + ", amplitude control "
                + vibrator.hasAmplitudeControl() + ", vibrator IDs " + manager.getVibratorIds().length);
        try {
            vibrator.vibrate(VibrationEffect.createOneShot(20, VibrationEffect.DEFAULT_AMPLITUDE));
            Log.i(mTag, "vibrated");
        } catch (SecurityException e) {
            Log.i(mTag, "vibrate: " + e.getClass().getName());
        }
        try {
            vibrator.cancel();
            Log.i(mTag, "cancelled");
        } catch (SecurityException e) {
            Log.i(mTag, "cancel: " + e.getClass().getName());
        }
    }

    /** A view's haptic feedback, which View sends to the vibrator service itself. */
    private void haptics() {
        final boolean haptic = mView.performHapticFeedback(HapticFeedbackConstants.LONG_PRESS);
        Log.i(mDevice, "haptic feedback for a long press " + haptic);
        Log.i(mTag, "haptic feedback for a long press sent");
    }

    /** The UI mode manager as the libraries ask it, from the application's context. */
    private void uiMode() {
        final Context application = getApplicationContext();
        final UiModeManager manager = (UiModeManager) application.getSystemService(Context.UI_MODE_SERVICE);
        Log.i(mTag, "ui mode manager: " + name(manager) + ", the same by class "
                + (manager == application.getSystemService(UiModeManager.class)));
        if (manager == null) {
            return;
        }
        final int uiMode = getResources().getConfiguration().uiMode;
        Log.i(mTag, "current mode type " + manager.getCurrentModeType() + ", the configuration's "
                + (uiMode & Configuration.UI_MODE_TYPE_MASK));
        final int night = manager.getNightMode();
        final int configured = uiMode & Configuration.UI_MODE_NIGHT_MASK;
        Log.i(mDevice, "night mode " + night + ", the configuration's " + configured);
        final boolean agrees = night == UiModeManager.MODE_NIGHT_YES ? configured == Configuration.UI_MODE_NIGHT_YES
                : night == UiModeManager.MODE_NIGHT_NO && configured == Configuration.UI_MODE_NIGHT_NO;
        Log.i(mTag, "night mode as the configuration's " + agrees);
        Log.i(mTag, "contrast " + manager.getContrast());
    }

    /** The captioning manager as media3 asks it, from the application's context. */
    private void captioning() {
        final Context application = getApplicationContext();
        final CaptioningManager manager = (CaptioningManager) application.getSystemService(Context.CAPTIONING_SERVICE);
        Log.i(mTag, "captioning manager: " + name(manager) + ", the same by class "
                + (manager == application.getSystemService(CaptioningManager.class)));
        if (manager == null) {
            return;
        }
        Log.i(mTag, "captions enabled " + manager.isEnabled() + ", locale " + manager.getLocale() + ", font scale "
                + manager.getFontScale());
        final CaptioningManager.CaptionStyle style = manager.getUserStyle();
        Log.i(mTag, "caption style: foreground " + Integer.toHexString(style.foregroundColor) + ", background "
                + Integer.toHexString(style.backgroundColor) + ", window " + Integer.toHexString(style.windowColor)
                + ", edge type " + style.edgeType + ", edge " + Integer.toHexString(style.edgeColor) + ", typeface "
                + style.getTypeface());
        Log.i(mTag, "caption style has: foreground " + style.hasForegroundColor() + ", background "
                + style.hasBackgroundColor() + ", window " + style.hasWindowColor() + ", edge type "
                + style.hasEdgeType() + ", edge " + style.hasEdgeColor());
    }

    /** The media metrics manager as media3's MediaMetricsListener asks it, from the application's context. */
    private void mediaMetrics() {
        final Context application = getApplicationContext();
        final MediaMetricsManager manager =
                (MediaMetricsManager) application.getSystemService(Context.MEDIA_METRICS_SERVICE);
        Log.i(mTag, "media metrics manager: " + name(manager) + ", the same by class "
                + (manager == application.getSystemService(MediaMetricsManager.class)));
        if (manager == null) {
            return;
        }
        final PlaybackSession session = manager.createPlaybackSession();
        final LogSessionId id = session.getSessionId();
        Log.i(mDevice, "playback session ID " + id.getStringId());
        Log.i(mTag, "playback session: " + name(session) + ", its ID none "
                + id.equals(LogSessionId.LOG_SESSION_ID_NONE) + ", " + id.getStringId().length() + " characters");
        session.reportPlaybackStateEvent(new PlaybackStateEvent.Builder()
                .setState(PlaybackStateEvent.STATE_PLAYING).setTimeSinceCreatedMillis(1).build());
        Log.i(mTag, "playback state reported");
        session.close();
        Log.i(mTag, "playback session closed");
    }
}
