package io.github.kisame76.minato.test.systemservices;

import android.app.Activity;
import android.app.DownloadManager;
import android.app.GameManager;
import android.app.KeyguardManager;
import android.app.SearchManager;
import android.app.UiModeManager;
import android.app.admin.DevicePolicyManager;
import android.app.usage.UsageStatsManager;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.RestrictionsManager;
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;
import android.content.res.Configuration;
import android.hardware.ConsumerIrManager;
import android.hardware.biometrics.BiometricManager;
import android.hardware.usb.UsbManager;
import android.media.metrics.LogSessionId;
import android.media.metrics.MediaMetricsManager;
import android.media.metrics.PlaybackSession;
import android.media.metrics.PlaybackStateEvent;
import android.net.Uri;
import android.os.Bundle;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.util.Log;
import android.view.HapticFeedbackConstants;
import android.view.View;
import android.view.accessibility.CaptioningManager;
import android.view.autofill.AutofillManager;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Gap 1's batch 3 (the compatibility plan): system services that answered null in Minato, each asked as the libraries
 * that call it ask, once the window has focus, so that its views are attached:
 * <ul>
 *   <li>the vibrator, by name and by class, and the vibrator manager: their classes, the same object by name and by
 *       class, a one-shot vibration and its cancel with the VIBRATE permission, and a view's haptic feedback for a long
 *       press, which View sends to the vibrator itself (use_vibrator_haptic_feedback), and the answers for all of
 *       two effects and two primitives against those for each. Whether the device has a vibrator, its amplitude
 *       control, its IDs, the effects, primitives and frequencies it supports and the haptic feedback's answer differ
 *       by device and are shown on the device tag.
 *   <li>the UI mode manager, from the application's context as media3's Util.isTv and AppCompat's night mode ask it:
 *       its class, the same object by class, the current mode type, the night mode as the configuration has it, and
 *       the contrast Material's ColorContrast reads. The night mode itself follows the device's dark theme and is shown
 *       on the device tag.
 *   <li>the captioning manager as media3's TrackSelectionParameters and SubtitleView ask it: its class, the same
 *       object by class, whether captions are on, their locale, font scale and the user's caption style;
 *   <li>the media metrics manager as media3's MediaMetricsListener asks it: its class, the same object by class, a
 *       playback session, a state event reported to it and its close. The session's ID, random, is on the device tag.
 *   <li>the autofill manager as Compose's AndroidAutofill uses it, which throws where a context gives none: its class,
 *       the same object by name, whether autofill is on, a callback registered and unregistered, and a view's
 *       entry, value change and exit, a request, a commit and a cancel sent to it. Whether the device supports
 *       autofill, which needs its autofill service, is on the device tag.
 *   <li>the shortcut manager as androidx's ShortcutManagerCompat uses it: its class, the same object by name, its
 *       limits, and dynamic shortcuts set, added, pushed, removed, used and updated, each list as the manager gives
 *       it back (ID, label, rank, activity), and what it refuses: more than its maximum and a shortcut without a
 *       label. Whether the launcher pins shortcuts and the icons' size in pixels differ by device.
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
        autofill();
        try {
            shortcuts();
        } catch (NoClassDefFoundError e) {
            Log.i(mTag, "shortcuts: " + e.getClass().getName());
        }
        game();
        keyguard();
        usb();
        download();
        usageStats();
        smallServices();
        devicePolicy();
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
        capabilities(vibrator);
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

    /**
     * The vibrator's effects and primitives as games ask them before a haptic: what it supports differs by device, and
     * Vibrator's answers for all of them follow its answers for each.
     */
    private void capabilities(Vibrator vibrator) {
        final int[] effects = {VibrationEffect.EFFECT_CLICK, VibrationEffect.EFFECT_TICK};
        final int[] primitives =
                {VibrationEffect.Composition.PRIMITIVE_CLICK, VibrationEffect.Composition.PRIMITIVE_TICK};
        final int[] effectsSupported = vibrator.areEffectsSupported(effects);
        final boolean[] primitivesSupported = vibrator.arePrimitivesSupported(primitives);
        final int[] durations = vibrator.getPrimitiveDurations(primitives);
        Log.i(mDevice, "effects " + Arrays.toString(effectsSupported) + ", primitives "
                + Arrays.toString(primitivesSupported) + ", durations " + Arrays.toString(durations)
                + ", resonant frequency " + vibrator.getResonantFrequency() + ", Q factor " + vibrator.getQFactor()
                + ", ID " + vibrator.getId());
        // Vibrator.areAllEffectsSupported's rule: no if one is not supported, else unknown if one is unknown.
        int all = Vibrator.VIBRATION_EFFECT_SUPPORT_YES;
        for (int support : effectsSupported) {
            if (support == Vibrator.VIBRATION_EFFECT_SUPPORT_NO) {
                all = support;
                break;
            }
            if (support != Vibrator.VIBRATION_EFFECT_SUPPORT_YES) {
                all = Vibrator.VIBRATION_EFFECT_SUPPORT_UNKNOWN;
            }
        }
        boolean allPrimitives = true;
        for (boolean supported : primitivesSupported) {
            allPrimitives &= supported;
        }
        Log.i(mTag, effectsSupported.length + " effects, " + primitivesSupported.length + " primitives, "
                + durations.length + " durations; all effects as each effect's "
                + (vibrator.areAllEffectsSupported(effects) == all) + ", all primitives as each primitive's "
                + (vibrator.areAllPrimitivesSupported(primitives) == allPrimitives));
    }

    /** A view's haptic feedback, which View sends to the vibrator service itself. */
    private void haptics() {
        final boolean haptic = mView.performHapticFeedback(HapticFeedbackConstants.LONG_PRESS);
        Log.i(mDevice, "haptic feedback for a long press " + haptic);
        Log.i(mTag, "haptic feedback for a long press sent");
    }

    /** The UI mode manager as the libraries ask it, from the application's context. */
    /** The game manager as an app asks it: no game, so no game mode (batch A of gap 1). */
    private void game() {
        final Context application = getApplicationContext();
        final GameManager manager = (GameManager) application.getSystemService(Context.GAME_SERVICE);
        Log.i(mTag, "game manager: " + name(manager) + ", the same by class "
                + (manager == application.getSystemService(GameManager.class)));
        if (manager == null) {
            return;
        }
        Log.i(mTag, "game mode " + manager.getGameMode());
    }

    /** The keyguard manager as ad SDKs and key stores ask it: no secure lock screen (batch A of gap 1). */
    @SuppressWarnings("deprecation")
    private void keyguard() {
        final Context application = getApplicationContext();
        final KeyguardManager manager = (KeyguardManager) application.getSystemService(Context.KEYGUARD_SERVICE);
        Log.i(mTag, "keyguard manager: " + name(manager) + ", the same by class "
                + (manager == application.getSystemService(KeyguardManager.class)));
        if (manager == null) {
            return;
        }
        Log.i(mTag, "keyguard locked " + manager.isKeyguardLocked() + ", secure " + manager.isKeyguardSecure()
                + ", restricted input " + manager.inKeyguardRestrictedInputMode());
        Log.i(mTag, "device locked " + manager.isDeviceLocked() + ", secure " + manager.isDeviceSecure());
        Log.i(mTag, "confirm credential intent " + manager.createConfirmDeviceCredentialIntent("title", "text"));
    }

    /** The USB manager as SDL and controller libraries ask it: nothing plugged in (batch A, step 2). */
    private void usb() {
        final Context application = getApplicationContext();
        final UsbManager manager = (UsbManager) application.getSystemService(Context.USB_SERVICE);
        Log.i(mTag, "usb manager: " + name(manager) + ", the same by class "
                + (manager == application.getSystemService(UsbManager.class)));
        if (manager == null) {
            return;
        }
        Log.i(mTag, "usb devices " + manager.getDeviceList().size() + ", accessories "
                + (manager.getAccessoryList() == null ? "none" : "some"));
    }

    /** The download manager as a WebView's download listener asks it (batch A, step 2). */
    private void download() {
        final Context application = getApplicationContext();
        final DownloadManager manager = (DownloadManager) application.getSystemService(Context.DOWNLOAD_SERVICE);
        Log.i(mTag, "download manager: " + name(manager) + ", the same by class "
                + (manager == application.getSystemService(DownloadManager.class)));
    }

    /** The usage stats manager as analytics libraries ask it, without usage access (batch A, step 2). */
    private void usageStats() {
        final Context application = getApplicationContext();
        final UsageStatsManager manager =
                (UsageStatsManager) application.getSystemService(Context.USAGE_STATS_SERVICE);
        Log.i(mTag, "usage stats manager: " + name(manager) + ", the same by class "
                + (manager == application.getSystemService(UsageStatsManager.class)));
        if (manager == null) {
            return;
        }
        final long now = System.currentTimeMillis();
        Log.i(mTag, "usage stats " + manager.queryUsageStats(UsageStatsManager.INTERVAL_DAILY, now - 86400000L, now)
                .size());
    }

    /** The small managers that libraries ask at start: restrictions, search, IR, biometrics. */
    @SuppressWarnings("deprecation")
    private void smallServices() {
        final Context application = getApplicationContext();
        final RestrictionsManager restrictions =
                (RestrictionsManager) application.getSystemService(Context.RESTRICTIONS_SERVICE);
        Log.i(mTag, "restrictions manager: " + name(restrictions) + ", the same by class "
                + (restrictions == application.getSystemService(RestrictionsManager.class)));
        if (restrictions != null) {
            final Bundle bundle = restrictions.getApplicationRestrictions();
            Log.i(mTag, "restrictions provider " + restrictions.hasRestrictionsProvider()
                    + ", application restrictions " + (bundle == null ? "null" : String.valueOf(bundle.size())));
        }
        final SearchManager search = (SearchManager) application.getSystemService(Context.SEARCH_SERVICE);
        Log.i(mTag, "search manager: " + name(search) + ", the same by class "
                + (search == application.getSystemService(SearchManager.class)));
        if (search != null) {
            Log.i(mTag, "searchable info " + search.getSearchableInfo(getComponentName()));
        }
        final ConsumerIrManager ir = (ConsumerIrManager) application.getSystemService(Context.CONSUMER_IR_SERVICE);
        Log.i(mTag, "consumer ir manager: " + name(ir) + ", the same by class "
                + (ir == application.getSystemService(ConsumerIrManager.class)));
        if (ir != null) {
            Log.i(mDevice, "has ir emitter " + ir.hasIrEmitter());
        }
        final BiometricManager biometric = (BiometricManager) application.getSystemService(Context.BIOMETRIC_SERVICE);
        Log.i(mTag, "biometric manager: " + name(biometric) + ", the same by class "
                + (biometric == application.getSystemService(BiometricManager.class)));
        if (biometric != null) {
            Log.i(mDevice, "can authenticate weak "
                    + answer(() -> biometric.canAuthenticate(BiometricManager.Authenticators.BIOMETRIC_WEAK))
                    + ", device credential "
                    + answer(() -> biometric.canAuthenticate(BiometricManager.Authenticators.DEVICE_CREDENTIAL)));
        }
    }

    /** One call's answer, or the exception it throws. */
    private static String answer(java.util.concurrent.Callable<?> call) {
        try {
            return String.valueOf(call.call());
        } catch (Exception e) {
            return e.getClass().getName();
        }
    }

    /** The device policy manager as libraries ask it on a device with no admin, owner or managed profile. */
    private void devicePolicy() {
        final Context application = getApplicationContext();
        final DevicePolicyManager manager =
                (DevicePolicyManager) application.getSystemService(Context.DEVICE_POLICY_SERVICE);
        Log.i(mTag, "device policy manager: " + name(manager) + ", the same by class "
                + (manager == application.getSystemService(DevicePolicyManager.class)));
        if (manager == null) {
            return;
        }
        final ComponentName admin = getComponentName();
        final String packageName = getPackageName();
        Log.i(mTag, "admin active " + answer(() -> manager.isAdminActive(admin)) + ", active admins "
                + answer(() -> manager.getActiveAdmins()));
        Log.i(mTag, "device owner app " + answer(() -> manager.isDeviceOwnerApp(packageName)) + ", profile owner app "
                + answer(() -> manager.isProfileOwnerApp(packageName)));
        Log.i(mTag, "camera disabled " + answer(() -> manager.getCameraDisabled(null)) + ", keyguard disabled features "
                + answer(() -> manager.getKeyguardDisabledFeatures(null)));
        Log.i(mTag, "password quality " + answer(() -> manager.getPasswordQuality(null)) + ", complexity "
                + answer(() -> manager.getPasswordComplexity()));
        Log.i(mTag, "storage encryption status " + answer(() -> manager.getStorageEncryptionStatus())
                + ", active password sufficient " + answer(() -> manager.isActivePasswordSufficient()));
        Log.i(mTag, "lock task permitted " + answer(() -> manager.isLockTaskPermitted(packageName))
                + ", application hidden " + answer(() -> manager.isApplicationHidden(admin, packageName)));
        Log.i(mTag, "lock now " + answer(() -> {
            manager.lockNow();
            return "ok";
        }));
    }

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

    /** The autofill manager as Compose's AndroidAutofill uses it, from the view's context. */
    private void autofill() {
        final Context context = mView.getContext();
        final AutofillManager manager = context.getSystemService(AutofillManager.class);
        Log.i(mTag, "autofill manager: " + name(manager) + ", the same by name "
                + (manager == context.getSystemService(Context.AUTOFILL_MANAGER_SERVICE)));
        if (manager == null) {
            return;
        }
        Log.i(mDevice, "autofill supported " + manager.isAutofillSupported());
        Log.i(mTag, "autofill enabled " + manager.isEnabled() + ", enabled services "
                + manager.hasEnabledAutofillServices() + ", service "
                + manager.getAutofillServiceComponentName() + ", field classification "
                + manager.isFieldClassificationEnabled());
        final AutofillManager.AutofillCallback callback = new AutofillManager.AutofillCallback() {
        };
        manager.registerCallback(callback);
        Log.i(mTag, "autofill callback registered");
        manager.notifyViewEntered(mView);
        manager.notifyValueChanged(mView);
        manager.notifyViewExited(mView);
        manager.requestAutofill(mView);
        manager.commit();
        manager.cancel();
        Log.i(mTag, "autofill told of the view's entry, change and exit, asked, committed and cancelled");
        manager.unregisterCallback(callback);
        Log.i(mTag, "autofill callback unregistered");
    }

    private ShortcutInfo shortcut(String id, String label) {
        return new ShortcutInfo.Builder(this, id).setShortLabel(label)
                .setIntent(new Intent(Intent.ACTION_VIEW, Uri.parse("https://example.org/" + id))).build();
    }

    /** The dynamic shortcuts as the manager gives them back: ID, label, rank and activity of each. */
    private void logShortcuts(ShortcutManager manager, String what) {
        final StringBuilder line = new StringBuilder("shortcuts " + what + ":");
        for (ShortcutInfo info : manager.getDynamicShortcuts()) {
            line.append(' ').append(info.getId()).append('/').append(info.getShortLabel()).append('/')
                    .append(info.getRank()).append('/').append(info.isDynamic() ? "dynamic" : "not dynamic")
                    .append('/').append(info.getActivity() == null ? "null" : info.getActivity().getClassName());
        }
        Log.i(mTag, line.toString());
    }

    /** The shortcut manager as androidx's ShortcutManagerCompat uses it. */
    private void shortcuts() {
        final ShortcutManager manager = getSystemService(ShortcutManager.class);
        Log.i(mTag, "shortcut manager: " + name(manager) + ", the same by name "
                + (manager == getSystemService(Context.SHORTCUT_SERVICE)));
        if (manager == null) {
            return;
        }
        final float density = getResources().getDisplayMetrics().density;
        Log.i(mDevice, "pinning supported " + manager.isRequestPinShortcutSupported() + ", icons at most "
                + manager.getIconMaxWidth() + " x " + manager.getIconMaxHeight() + " pixels");
        Log.i(mTag, "shortcuts at most " + manager.getMaxShortcutCountPerActivity() + " per activity, rate limiting "
                + manager.isRateLimitingActive() + ", icons at most " + Math.round(manager.getIconMaxWidth() / density)
                + " x " + Math.round(manager.getIconMaxHeight() / density) + " dp, manifest "
                + manager.getManifestShortcuts().size() + ", pinned " + manager.getPinnedShortcuts().size());
        Log.i(mTag, "set "
                + manager.setDynamicShortcuts(Arrays.asList(shortcut("one", "One"), shortcut("two", "Two"))));
        logShortcuts(manager, "set");
        Log.i(mTag, "added " + manager.addDynamicShortcuts(Arrays.asList(shortcut("three", "Three"))));
        logShortcuts(manager, "added");
        manager.pushDynamicShortcut(shortcut("four", "Four"));
        logShortcuts(manager, "pushed");
        manager.removeDynamicShortcuts(Arrays.asList("one"));
        logShortcuts(manager, "removed");
        manager.reportShortcutUsed("two");
        Log.i(mTag, "updated " + manager.updateShortcuts(Arrays.asList(shortcut("two", "Second"))));
        logShortcuts(manager, "updated");
        final List<ShortcutInfo> many = new ArrayList<>();
        for (int i = 0; i <= manager.getMaxShortcutCountPerActivity(); i++) {
            many.add(shortcut("many" + i, "Many " + i));
        }
        try {
            manager.setDynamicShortcuts(many);
            Log.i(mTag, "set one more than the maximum");
        } catch (IllegalArgumentException e) {
            Log.i(mTag, "one more than the maximum: " + e.getClass().getName());
        }
        try {
            manager.addDynamicShortcuts(Arrays.asList(new ShortcutInfo.Builder(this, "unlabelled")
                    .setIntent(new Intent(Intent.ACTION_VIEW)).build()));
            Log.i(mTag, "added a shortcut without a label");
        } catch (IllegalArgumentException e) {
            Log.i(mTag, "a shortcut without a label: " + e.getClass().getName());
        }
        logShortcuts(manager, "after the refusals");
        manager.removeAllDynamicShortcuts();
        logShortcuts(manager, "all removed");
    }
}
