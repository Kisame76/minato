package io.github.kisame76.minato.test.servicesb;

import android.app.Application;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.Collection;
import java.util.concurrent.Callable;

/**
 * Ten system services that apps ask at start: each is asked for by name and by class, and a few harmless calls are
 * logged with their result, or with the class of the exception they throw. The app holds no permission of these
 * services, so what needs one shows Android's SecurityException. The classes and methods are reached by name, so that
 * the same sources build against Android's SDK and against Minato's framework before it has the classes.
 */
public class ServicesBApplication extends Application {
    private static final String TAG = "services-b-test";

    private static void log(String name, Callable<Object> call) {
        String result;
        try {
            final Object value = call.call();
            result = value instanceof Collection ? "size " + ((Collection<?>) value).size() : String.valueOf(value);
        } catch (InvocationTargetException e) {
            result = "throws " + e.getCause().getClass().getName();
        } catch (Throwable e) {
            result = "throws " + e.getClass().getName();
        }
        Log.i(TAG, name + ": " + result);
    }

    private static Object call(Object target, String method, Object... args) throws Exception {
        for (Method candidate : target.getClass().getMethods()) {
            if (candidate.getName().equals(method) && candidate.getParameterTypes().length == args.length) {
                return candidate.invoke(target, args);
            }
        }
        throw new NoSuchMethodException(target.getClass().getName() + "." + method);
    }

    private static Object callStatic(String type, String method, Object... args) throws Exception {
        for (Method candidate : Class.forName(type).getMethods()) {
            if (candidate.getName().equals(method) && candidate.getParameterTypes().length == args.length) {
                return candidate.invoke(null, args);
            }
        }
        throw new NoSuchMethodException(type + "." + method);
    }

    private Object present(String name, String type) {
        final Object byName = getSystemService(name);
        Object byClass = null;
        try {
            byClass = getSystemService(Class.forName(type));
        } catch (ClassNotFoundException e) {
            Log.i(TAG, type.substring(type.lastIndexOf('.') + 1) + " class: missing");
        }
        Log.i(TAG, type.substring(type.lastIndexOf('.') + 1) + " by name: " + (byName != null)
                + ", by class: " + (byClass != null) + ", same: " + (byName == byClass));
        return byName;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        new Handler(Looper.getMainLooper()).post(this::run);
    }

    private void run() {
        final Object sessions = present(Context.MEDIA_SESSION_SERVICE, "android.media.session.MediaSessionManager");
        final Object router = present(Context.MEDIA_ROUTER_SERVICE, "android.media.MediaRouter");
        final Object bluetooth = present(Context.BLUETOOTH_SERVICE, "android.bluetooth.BluetoothManager");
        final Object subscriptions = present(Context.TELEPHONY_SUBSCRIPTION_SERVICE,
                "android.telephony.SubscriptionManager");
        final Object telecom = present(Context.TELECOM_SERVICE, "android.telecom.TelecomManager");
        final Object carrier = present(Context.CARRIER_CONFIG_SERVICE, "android.telephony.CarrierConfigManager");
        final Object nfc = present(Context.NFC_SERVICE, "android.nfc.NfcManager");
        final Object midi = present(Context.MIDI_SERVICE, "android.media.midi.MidiManager");
        final Object projection = present(Context.MEDIA_PROJECTION_SERVICE,
                "android.media.projection.MediaProjectionManager");
        final Object roles = present(Context.ROLE_SERVICE, "android.app.role.RoleManager");

        if (sessions != null) {
            log("MediaSessionManager.getActiveSessions(null)",
                    () -> call(sessions, "getActiveSessions", (Object) null));
        }
        if (router != null) {
            log("MediaRouter.getSelectedRoute(LIVE_AUDIO) is not null",
                    () -> call(router, "getSelectedRoute", 1) != null);
            log("MediaRouter.getRouteCount() > 0", () -> (Integer) call(router, "getRouteCount") > 0);
        }
        if (bluetooth != null) {
            log("BluetoothManager.getAdapter() is null", () -> call(bluetooth, "getAdapter") == null);
            log("BluetoothManager.getConnectedDevices(GATT)", () -> call(bluetooth, "getConnectedDevices", 7));
        }
        if (subscriptions != null) {
            log("SubscriptionManager.getActiveSubscriptionInfoCount()",
                    () -> call(subscriptions, "getActiveSubscriptionInfoCount"));
            log("SubscriptionManager.getDefaultSubscriptionId()",
                    () -> callStatic("android.telephony.SubscriptionManager", "getDefaultSubscriptionId"));
            log("SubscriptionManager.isValidSubscriptionId(-1)",
                    () -> callStatic("android.telephony.SubscriptionManager", "isValidSubscriptionId", -1));
        }
        if (telecom != null) {
            log("TelecomManager.isInCall()", () -> call(telecom, "isInCall"));
            log("TelecomManager.getCallCapablePhoneAccounts()", () -> call(telecom, "getCallCapablePhoneAccounts"));
        }
        if (carrier != null) {
            log("CarrierConfigManager.getConfig() is null", () -> call(carrier, "getConfig") == null);
        }
        if (nfc != null) {
            log("NfcManager.getDefaultAdapter() is null", () -> call(nfc, "getDefaultAdapter") == null);
        }
        if (midi != null) {
            log("MidiManager.getDevices().length", () -> ((Object[]) call(midi, "getDevices")).length);
        }
        if (projection != null) {
            log("MediaProjectionManager.createScreenCaptureIntent() is not null",
                    () -> call(projection, "createScreenCaptureIntent") != null);
        }
        if (roles != null) {
            log("RoleManager.isRoleAvailable(HOME)", () -> call(roles, "isRoleAvailable", "android.app.role.HOME"));
            log("RoleManager.isRoleHeld(HOME)", () -> call(roles, "isRoleHeld", "android.app.role.HOME"));
        }
        Log.i(TAG, "done");
    }
}
