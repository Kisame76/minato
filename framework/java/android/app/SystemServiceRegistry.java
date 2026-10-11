/*
 * Our SystemServiceRegistry, under AOSP's name (ADR-0045 decision 2): the services a context gives, by name and by
 * class, each made once per context as the tag's CachedServiceFetcher keeps them in ContextImpl's mServiceCache, or
 * once per call where the tag's fetcher is static (the input method manager's forContext). It replaces ContextImpl's
 * two lists of the eight services it had (gap 1 of the compatibility plan, batch 1) and adds the activity, display,
 * power, audio and connectivity services, with power exemptions, which PowerManager asks, the notification service
 * (batch 2) and the job scheduler (gap 9). A name or class without an
 * entry is logged once and gives null, as the tag's onServiceNotFound does; so does a service whose local binder the
 * table lacks (ServiceNotFoundException). AOSP's registry would load the mainline modules' classes and register 141
 * services; batches 2 and 3 add the rest.
 */
package android.app;

import android.accounts.AccountManager;
import android.accounts.IAccountManager;
import android.app.job.IJobScheduler;
import android.app.job.JobScheduler;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.res.Resources;
import android.hardware.SystemSensorManager;
import android.hardware.SensorManager;
import android.hardware.display.DisplayManager;
import android.hardware.input.InputManager;
import android.media.AudioManager;
import android.net.ConnectivityManager;
import android.net.wifi.WifiManager;
import android.os.IPowerManager;
import android.os.IThermalService;
import android.os.IUserManager;
import android.os.Looper;
import android.os.PowerExemptionManager;
import android.os.PowerManager;
import android.os.ServiceManager;
import android.os.ServiceManager.ServiceNotFoundException;
import android.os.UserManager;
import android.telephony.TelephonyManager;
import android.os.storage.StorageManager;
import android.util.ArrayMap;
import android.view.ContextThemeWrapper;
import android.view.LayoutInflater;
import android.view.WindowManager;
import android.view.WindowManagerImpl;
import android.view.inputmethod.InputMethodManager;

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

    static {
        cached(Context.LAYOUT_INFLATER_SERVICE, LayoutInflater.class,
                ctx -> new PhoneLayoutInflater(ctx.getOuterContext()));
        cached(Context.WINDOW_SERVICE, WindowManager.class, ctx -> new WindowManagerImpl((Context) (Object) ctx));
        cached(Context.SENSOR_SERVICE, SensorManager.class,
                ctx -> new SystemSensorManager(ctx.getOuterContext(), Looper.getMainLooper()));
        cached(Context.WIFI_SERVICE, WifiManager.class, ctx -> new WifiManager(ctx.getOuterContext()));
        cached(Context.INPUT_SERVICE, InputManager.class, ctx -> new InputManager(ctx.getOuterContext()));
        cached(Context.STORAGE_SERVICE, StorageManager.class,
                ctx -> new StorageManager((Context) (Object) ctx, Looper.getMainLooper()));
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
        // Gap 1: as the tag's, over the local clipboard.
        cached(Context.CLIPBOARD_SERVICE, ClipboardManager.class,
                ctx -> new ClipboardManager(ctx.getOuterContext(), ctx.mMainThread.getHandler()));
        // Retest 12: the app's and the system's languages (Helpshift), as the tag's over the outer context.
        cached(Context.LOCALE_SERVICE, LocaleManager.class, ctx -> new LocaleManager(ctx.getOuterContext()));
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
