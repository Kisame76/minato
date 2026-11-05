/*
 * Our SystemServiceRegistry, under AOSP's name (ADR-0045 decision 2): the services a context gives, by name and by
 * class, each made once per context as the tag's CachedServiceFetcher keeps them in ContextImpl's mServiceCache, or
 * once per call where the tag's fetcher is static (the input method manager's forContext). It replaces ContextImpl's
 * two lists of the eight services it had (gap 1 of the compatibility plan, batch 1) and adds the activity, display,
 * power, audio and connectivity services, with power exemptions, which PowerManager asks, the notification service
 * (batch 2), the job scheduler (gap 9), and the vibrator with its manager and the UI mode, captioning, media
 * metrics, autofill and shortcut managers (batch 3), and the battery manager. A name or class without an entry is
 * logged once and gives null,
 * as the tag's onServiceNotFound does; so does a service whose local binder the table lacks
 * (ServiceNotFoundException). AOSP's registry would load the mainline modules' classes and register 141 services;
 * batches 2 and 3 add the rest.
 */
package android.app;

import android.location.ILocationManager;
import android.location.LocationManager;
import android.accounts.AccountManager;
import android.accounts.IAccountManager;
import android.app.job.IJobScheduler;
import android.app.job.JobScheduler;
import android.appwidget.AppWidgetManager;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.pm.ShortcutManager;
import android.content.res.Resources;
import android.hardware.SystemSensorManager;
import android.hardware.camera2.CameraManager;
import android.hardware.SensorManager;
import android.hardware.display.DisplayManager;
import android.app.usage.IUsageStatsManager;
import android.app.usage.UsageStatsManager;
import android.app.admin.DevicePolicyManager;
import android.content.pm.LauncherApps;
import android.content.IRestrictionsManager;
import android.content.RestrictionsManager;
import android.hardware.ConsumerIrManager;
import android.hardware.biometrics.BiometricManager;
import android.hardware.input.InputManager;
import android.hardware.usb.IUsbManager;
import android.hardware.usb.UsbManager;
import android.media.AudioManager;
import android.media.metrics.IMediaMetricsManager;
import android.media.metrics.MediaMetricsManager;
import android.net.ConnectivityManager;
import android.net.wifi.WifiManager;
import android.os.BatteryManager;
import android.os.BatteryStats;
import android.os.IBatteryPropertiesRegistrar;
import android.os.IPowerManager;
import android.os.IThermalService;
import android.os.IUserManager;
import android.os.Looper;
import android.os.PowerExemptionManager;
import android.os.PowerManager;
import android.os.ServiceManager;
import android.os.ServiceManager.ServiceNotFoundException;
import android.os.SystemVibrator;
import android.os.SystemVibratorManager;
import android.os.UserManager;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.telephony.TelephonyManager;
import android.app.usage.StorageStatsManager;
import android.os.storage.StorageManager;
import android.util.ArrayMap;
import android.view.ContextThemeWrapper;
import android.view.LayoutInflater;
import android.view.WindowManager;
import android.view.WindowManagerImpl;
import android.view.accessibility.AccessibilityManager;
import android.view.contentcapture.ContentCaptureManager;
import android.view.accessibility.CaptioningManager;
import android.view.autofill.AutofillManager;
import android.view.inputmethod.InputMethodManager;
import android.view.textclassifier.TextClassificationManager;

import com.android.internal.app.IBatteryStats;
import com.android.internal.policy.PhoneLayoutInflater;
import com.android.minato.services.Unwritten;

final class SystemServiceRegistry {
    /**
     * Makes a context's service; a static one (cached false) is made at each call, as the tag's StaticServiceFetcher.
     */
    private interface Maker {
        Object make(ContextImpl ctx) throws ServiceNotFoundException;
    }

    private static final class Fetcher {
        final int index;
        final boolean cached;
        final Maker maker;

        Fetcher(int index, boolean cached, Maker maker) {
            this.index = index;
            this.cached = cached;
            this.maker = maker;
        }
    }

    private static final ArrayMap<Class<?>, String> NAMES = new ArrayMap<>();
    private static final ArrayMap<String, Fetcher> FETCHERS = new ArrayMap<>();
    private static int sCacheSize;
    /** The vibrator's name, which the API deprecates for the vibrator manager's but still gives (gap 1, batch 3). */
    @SuppressWarnings("deprecation")
    private static final String VIBRATOR_SERVICE = Context.VIBRATOR_SERVICE;

    static {
        cached(Context.LAYOUT_INFLATER_SERVICE, LayoutInflater.class,
                ctx -> new PhoneLayoutInflater(ctx.getOuterContext()));
        cached(Context.WINDOW_SERVICE, WindowManager.class, ctx -> new WindowManagerImpl((Context) (Object) ctx));
        cached(Context.SENSOR_SERVICE, SensorManager.class,
                ctx -> new SystemSensorManager(ctx.getOuterContext(), Looper.getMainLooper()));
        cached(Context.APPWIDGET_SERVICE, AppWidgetManager.class,
                ctx -> new AppWidgetManager(ctx.getOuterContext(), null));
        cached(Context.ACCESSIBILITY_SERVICE, AccessibilityManager.class,
                ctx -> AccessibilityManager.getInstance(ctx.getOuterContext()));
        cached(Context.WIFI_SERVICE, WifiManager.class, ctx -> new WifiManager(ctx.getOuterContext()));
        cached(Context.INPUT_SERVICE, InputManager.class, ctx -> new InputManager(ctx.getOuterContext()));
        cached(Context.STORAGE_SERVICE, StorageManager.class,
                ctx -> new StorageManager((Context) (Object) ctx, Looper.getMainLooper()));
        // The free-space stats an engine may read instead of StorageManager.getAllocatableBytes (ADR-0045).
        cached(Context.STORAGE_STATS_SERVICE, StorageStatsManager.class,
                ctx -> new StorageStatsManager((Context) (Object) ctx));
        cached(Context.APP_OPS_SERVICE, AppOpsManager.class, ctx -> new AppOpsManager((Context) (Object) ctx));
        // The process's manager, without an input method (ADR-0043 step d)), as the tag's forContext.
        register(Context.INPUT_METHOD_SERVICE, InputMethodManager.class, false,
                ctx -> InputMethodManager.forContext(ctx.getOuterContext()));
        // Gap 1, batch 1.
        cached(Context.ACTIVITY_SERVICE, ActivityManager.class,
                ctx -> new ActivityManager(ctx.getOuterContext(), ctx.mMainThread.getHandler()));
        cached(Context.DISPLAY_SERVICE, DisplayManager.class, ctx -> new DisplayManager(ctx.getOuterContext()));
        cached(Context.POWER_SERVICE, PowerManager.class, ctx -> new PowerManager(ctx.getOuterContext(),
                IPowerManager.Stub.asInterface(ServiceManager.getServiceOrThrow(Context.POWER_SERVICE)),
                IThermalService.Stub.asInterface(ServiceManager.getServiceOrThrow(Context.THERMAL_SERVICE)),
                ctx.mMainThread.getHandler()));
        cached(Context.POWER_EXEMPTION_SERVICE, PowerExemptionManager.class,
                ctx -> new PowerExemptionManager(ctx.getOuterContext()));
        cached(Context.USER_SERVICE, UserManager.class, ctx -> new UserManager(ctx.getOuterContext(),
                IUserManager.Stub.asInterface(ServiceManager.getServiceOrThrow(Context.USER_SERVICE))));
        cached(Context.AUDIO_SERVICE, AudioManager.class, ctx -> new AudioManager((Context) (Object) ctx));
        cached(Context.CONNECTIVITY_SERVICE, ConnectivityManager.class,
                ctx -> new ConnectivityManager(ctx.getOuterContext()));
        // Gap 1, batch 2: as the tag's, over the dialog theme of the app's target SDK.
        cached(Context.NOTIFICATION_SERVICE, NotificationManager.class, ctx -> new NotificationManager(
                new ContextThemeWrapper(ctx.getOuterContext(), dialogTheme(ctx.getOuterContext())),
                ctx.mMainThread.getHandler()));
        // Gap 1: a phone without a SIM (ADR-0045 decision 4), as the tag's constructor takes the outer context.
        cached(Context.TELEPHONY_SERVICE, TelephonyManager.class, ctx -> new TelephonyManager(ctx.getOuterContext()));
        // The gap sort's category 2: as the tag's, on a device without cameras.
        cached(Context.CAMERA_SERVICE, CameraManager.class, ctx -> new CameraManager(ctx.getOuterContext()));
        // Gap 1: as the tag's, over the local clipboard.
        cached(Context.CLIPBOARD_SERVICE, ClipboardManager.class,
                ctx -> new ClipboardManager(ctx.getOuterContext(), ctx.mMainThread.getHandler()));
        // Retest 12: the app's and the system's languages (Helpshift), as the tag's over the outer context.
        cached(Context.LOCALE_SERVICE, LocaleManager.class, ctx -> new LocaleManager(ctx.getOuterContext()));
        // As the tag's: the text classification manager, whose classifier finds nothing.
        cached(Context.TEXT_CLASSIFICATION_SERVICE, TextClassificationManager.class,
                ctx -> new TextClassificationManager(ctx.getOuterContext()));
        // Retest 8's batch B2: as the tag's, over the local account service.
        cached(Context.ACCOUNT_SERVICE, AccountManager.class, ctx -> new AccountManager(ctx.getOuterContext(),
                IAccountManager.Stub.asInterface(ServiceManager.getServiceOrThrow(Context.ACCOUNT_SERVICE))));
        // Gap 9: as JobSchedulerFrameworkInitializer's, over the local scheduler.
        cached(Context.JOB_SCHEDULER_SERVICE, JobScheduler.class, ctx -> new JobSchedulerImpl(ctx.getOuterContext(),
                IJobScheduler.Stub.asInterface(ServiceManager.getServiceOrThrow(Context.JOB_SCHEDULER_SERVICE))));
        // Batch D 3: as JobSchedulerFrameworkInitializer's, over the local alarm service.
        cached(Context.ALARM_SERVICE, AlarmManager.class, ctx -> new AlarmManager(
                IAlarmManager.Stub.asInterface(ServiceManager.getServiceOrThrow(Context.ALARM_SERVICE)),
                ctx.getOuterContext()));
        // Batch 3: ours under AOSP's names, over the local vibrator manager service.
        cached(Context.VIBRATOR_MANAGER_SERVICE, VibratorManager.class,
                ctx -> new SystemVibratorManager((Context) (Object) ctx));
        cached(VIBRATOR_SERVICE, Vibrator.class, ctx -> new SystemVibrator((Context) (Object) ctx));
        // Batch 3: as the tag's, over the local UI mode service.
        cached(Context.UI_MODE_SERVICE, UiModeManager.class, ctx -> new UiModeManager(ctx.getOuterContext()));
        // Batch 3: as the tag's; the captioning manager reads the captions' settings itself.
        cached(Context.CAPTIONING_SERVICE, CaptioningManager.class,
                ctx -> new CaptioningManager(ctx.getOuterContext()));
        // Batch 3: as the tag's where the device has no autofill service.
        cached(Context.AUTOFILL_MANAGER_SERVICE, AutofillManager.class,
                ctx -> new AutofillManager(ctx.getOuterContext()));
        // Batch 3: as the tag's, over the local shortcut service.
        cached(Context.SHORTCUT_SERVICE, ShortcutManager.class, ctx -> new ShortcutManager(ctx.getOuterContext()));
        // Batch 3: as the tag's, over the local media metrics service.
        cached(Context.MEDIA_METRICS_SERVICE, MediaMetricsManager.class, ctx -> new MediaMetricsManager(
                IMediaMetricsManager.Stub.asInterface(ServiceManager.getServiceOrThrow(Context.MEDIA_METRICS_SERVICE)),
                ctx.getUserId()));
        // The battery: as the tag's, over the local battery services.
        cached(Context.BATTERY_SERVICE, BatteryManager.class, ctx -> new BatteryManager(ctx.getOuterContext(),
                IBatteryStats.Stub.asInterface(ServiceManager.getServiceOrThrow(BatteryStats.SERVICE_NAME)),
                IBatteryPropertiesRegistrar.Stub.asInterface(ServiceManager.getServiceOrThrow("batteryproperties"))));
        // Content capture, an optional feature: as the tag's, the name is registered and an app whose package no content
        // capture provider allowlists gets no manager (createService returns null), which no provider of Minato's does.
        cached(Context.CONTENT_CAPTURE_MANAGER_SERVICE, ContentCaptureManager.class, ctx -> null);
        // ADR-0063: as AOSP's, over the local location service.
        cached(Context.LOCATION_SERVICE, LocationManager.class, ctx -> new LocationManager(ctx.getOuterContext(),
                ILocationManager.Stub.asInterface(ServiceManager.getServiceOrThrow(Context.LOCATION_SERVICE))));
        // Gap 1, batch A: as the tag's, over the local game service.
        cached(Context.GAME_SERVICE, GameManager.class,
                ctx -> new GameManager(ctx.getOuterContext(), ctx.mMainThread.getHandler()));
        // Gap 1, batch A: ours under AOSP's name, on a device without a secure lock screen (see KeyguardManager).
        cached(Context.KEYGUARD_SERVICE, KeyguardManager.class, ctx -> new KeyguardManager(ctx.getOuterContext()));
        // Gap 1, batch A step 2: as the tag's, over the local USB service.
        cached(Context.USB_SERVICE, UsbManager.class, ctx -> new UsbManager(ctx,
                IUsbManager.Stub.asInterface(ServiceManager.getServiceOrThrow(Context.USB_SERVICE))));
        // Gap 1, batch A step 2: as the tag's.
        cached(Context.DOWNLOAD_SERVICE, DownloadManager.class, ctx -> new DownloadManager(ctx.getOuterContext()));
        // Gap 1, batch A step 2: as the tag's, over the local usage stats service.
        cached(Context.USAGE_STATS_SERVICE, UsageStatsManager.class, ctx -> new UsageStatsManager(ctx.getOuterContext(),
                IUsageStatsManager.Stub.asInterface(ServiceManager.getServiceOrThrow(Context.USAGE_STATS_SERVICE))));
        // Gap 1, batch B: as the tag's, over the local services of the small managers.
        cached(Context.RESTRICTIONS_SERVICE, RestrictionsManager.class, ctx -> new RestrictionsManager(ctx,
                IRestrictionsManager.Stub.asInterface(ServiceManager.getServiceOrThrow(Context.RESTRICTIONS_SERVICE))));
        // Generated, as managergen writes the public API of a manager with no hand file: answers of a phone with no
        // searchable activity.
        cached(Context.SEARCH_SERVICE, SearchManager.class,
                ctx -> new SearchManager(ctx.getOuterContext(), ctx.mMainThread.getHandler()));
        cached(Context.CONSUMER_IR_SERVICE, ConsumerIrManager.class, ctx -> new ConsumerIrManager(ctx));
        // Ours under AOSP's name, a phone without a biometric sensor (see BiometricManager).
        cached(Context.BIOMETRIC_SERVICE, BiometricManager.class, ctx -> new BiometricManager(ctx.getOuterContext()));
        // Gap 1, batch C: ours under AOSP's name, a device with no admin, owner or managed profile.
        cached(Context.DEVICE_POLICY_SERVICE, DevicePolicyManager.class,
                ctx -> new DevicePolicyManager(ctx.getOuterContext()));
        // Gap 1, batch D: ours under AOSP's name, generated with the written part of framework/managers
        // (WallpaperManager answers for an app without wallpaper permissions; LauncherApps names the app's own package
        // only).
        cached(Context.WALLPAPER_SERVICE, WallpaperManager.class, ctx -> new WallpaperManager(ctx.getOuterContext()));
        cached(Context.LAUNCHER_APPS_SERVICE, LauncherApps.class, ctx -> new LauncherApps(ctx.getOuterContext()));
    }

    private SystemServiceRegistry() {
    }

    /** The tag's theme of NotificationManager's context: the dialog theme of the app's target SDK. */
    @SuppressWarnings("deprecation") // Holo's dialog theme, which the tag picks for targets 11 to 13.
    private static int dialogTheme(Context outer) {
        return Resources.selectSystemTheme(0, outer.getApplicationInfo().targetSdkVersion,
                com.android.internal.R.style.Theme_Dialog, com.android.internal.R.style.Theme_Holo_Dialog,
                com.android.internal.R.style.Theme_DeviceDefault_Dialog,
                com.android.internal.R.style.Theme_DeviceDefault_Light_Dialog);
    }

    private static void cached(String name, Class<?> type, Maker maker) {
        register(name, type, true, maker);
    }

    private static void register(String name, Class<?> type, boolean cached, Maker maker) {
        NAMES.put(type, name);
        FETCHERS.put(name, new Fetcher(cached ? sCacheSize++ : -1, cached, maker));
    }

    /** A new context's cache, one slot per cached service, as the tag's createServiceCache. */
    static Object[] createServiceCache() {
        return new Object[sCacheSize];
    }

    /** The context's service of {@code name}, made at its first ask, or null for a name without one. */
    static Object getSystemService(ContextImpl ctx, String name) {
        final Fetcher fetcher = name != null ? FETCHERS.get(name) : null;
        if (fetcher == null) {
            Unwritten.log("android.app.ContextImpl.getSystemService(String) for \"" + name + "\"", "null");
            return null;
        }
        try {
            if (!fetcher.cached) {
                return fetcher.maker.make(ctx);
            }
            synchronized (ctx.mServiceCache) {
                Object service = ctx.mServiceCache[fetcher.index];
                if (service == null) {
                    service = fetcher.maker.make(ctx);
                    ctx.mServiceCache[fetcher.index] = service;
                }
                return service;
            }
        } catch (ServiceNotFoundException e) {
            Unwritten.log("android.app.ContextImpl.getSystemService(String) for \"" + name + "\" without its service "
                    + e.getMessage(), "null");
            return null;
        }
    }

    /** The name of the service of {@code type}, or null for a class without one. */
    static String getSystemServiceName(Class<?> type) {
        final String name = type != null ? NAMES.get(type) : null;
        if (name == null) {
            Unwritten.log("android.app.ContextImpl.getSystemServiceName(Class) for "
                    + (type == null ? "null" : type.getName()), "null");
        }
        return name;
    }
}
