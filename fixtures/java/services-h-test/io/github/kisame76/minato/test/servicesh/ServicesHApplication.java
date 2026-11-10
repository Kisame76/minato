package io.github.kisame76.minato.test.servicesh;

import android.app.Application;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.Comparator;
import java.util.List;
import java.util.Map;

/**
 * Eleven of the fifteen system services that the service inventory found without a manager in Minato (bundle B; app
 * search, device lock, health connect and media communication wait for their module API files): each is asked for
 * by name and by class, and every public zero-argument getter or predicate of its class (a name that starts with get,
 * is, has, can, supports or are) is called and logged in the order of its name, with a simple result (a primitive, a
 * string, the size of a collection, an array or a map, or the class of an object) or the class of the exception it
 * throws. The app holds no permission of these services, so what needs one shows Android's SecurityException.
 * Everything is reached by name, so that the same sources build against Android's SDK and against Minato's framework.
 */
public class ServicesHApplication extends Application {
    private static final String TAG = "services-h-test";
    private static final String[][] SERVICES = {
        {"app_search", "android.app.appsearch.AppSearchManager"},
        {"blob_store", "android.app.blob.BlobStoreManager"},
        {"bugreport", "android.os.BugreportManager"},
        {"companiondevice", "android.companion.CompanionDeviceManager"},
        {"credential", "android.credentials.CredentialManager"},
        {"crossprofileapps", "android.content.pm.CrossProfileApps"},
        {"device_lock", "android.devicelock.DeviceLockManager"},
        {"display_hash", "android.view.displayhash.DisplayHashManager"},
        {"domain_verification", "android.content.pm.verify.domain.DomainVerificationManager"},
        {"file_integrity", "android.security.FileIntegrityManager"},
        {"healthconnect", "android.health.connect.HealthConnectManager"},
        {"media_communication", "android.media.MediaCommunicationManager"},
        {"overlay", "android.content.om.OverlayManager"},
        {"people", "android.app.people.PeopleManager"},
        {"virtualdevice", "android.companion.virtual.VirtualDeviceManager"},
    };
    private static final String[] PREFIXES = {"get", "is", "has", "can", "supports", "are"};

    @Override
    public void onCreate() {
        super.onCreate();
        new Handler(Looper.getMainLooper()).post(this::run);
    }

    private static String describe(Object value) {
        if (value == null) {
            return "null";
        }
        if (value instanceof Collection) {
            return "size " + ((Collection<?>) value).size();
        }
        if (value instanceof Map) {
            return "size " + ((Map<?, ?>) value).size();
        }
        if (value.getClass().isArray()) {
            return "length " + java.lang.reflect.Array.getLength(value);
        }
        if (value instanceof Number || value instanceof Boolean || value instanceof CharSequence
                || value instanceof Enum) {
            return String.valueOf(value);
        }
        return "object " + value.getClass().getName();
    }

    private void run() {
        for (String[] service : SERVICES) {
            final String simple = service[1].substring(service[1].lastIndexOf('.') + 1);
            try {
                final Object byName = getSystemService(service[0]);
                Object byClass = null;
                try {
                    byClass = getSystemService(Class.forName(service[1]));
                } catch (ClassNotFoundException e) {
                    Log.i(TAG, simple + " class: missing");
                }
                Log.i(TAG, simple + " by name: " + (byName != null) + ", by class: " + (byClass != null)
                        + ", same: " + (byName == byClass));
                if (byName != null) {
                    getters(simple, byName);
                }
            } catch (Throwable t) {
                Log.i(TAG, simple + " failed: " + t.getClass().getName());
                Log.i(TAG + "-device", simple + ": " + Log.getStackTraceString(t));
            }
        }
        Log.i(TAG, "done");
    }

    private static void getters(String simple, Object manager) {
        final List<Method> methods = new ArrayList<>(Arrays.asList(manager.getClass().getMethods()));
        methods.sort(Comparator.comparing(Method::getName));
        for (Method method : methods) {
            if (method.getParameterTypes().length != 0 || Modifier.isStatic(method.getModifiers())
                    || method.getDeclaringClass() == Object.class || !wanted(method.getName())) {
                continue;
            }
            String result;
            try {
                result = describe(method.invoke(manager));
            } catch (InvocationTargetException e) {
                result = "throws " + e.getCause().getClass().getName();
            } catch (Throwable e) {
                result = "throws " + e.getClass().getName();
            }
            Log.i(TAG, simple + "." + method.getName() + "(): " + result);
        }
    }

    private static boolean wanted(String name) {
        for (String prefix : PREFIXES) {
            if (name.startsWith(prefix) && name.length() > prefix.length()
                    && Character.isUpperCase(name.charAt(prefix.length()))) {
                return !name.equals("getClass");
            }
        }
        return false;
    }
}
