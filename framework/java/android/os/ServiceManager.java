/*
 * Our ServiceManager, under AOSP's name (ADR-0045 decision 2): one table of the process's local services, from a name
 * to a Binder that carries its service through attachInterface, so that AOSP's lookups,
 * IFoo.Stub.asInterface(ServiceManager.getService(name)), find the service through queryLocalInterface without IPC.
 * A service is made at its first lookup, under the table's lock, and every caller then gets the same Binder; a
 * service that looks up another while it is made takes the lock again. An unknown name gives null, as Android's
 * getService does for a service that does not exist, and is logged once per name. The table's first entry is
 * "display", DisplayManagerGlobal's (ADR-0041 a)), the second "platform_compat", CompatChanges' (ADR-0039 step c2e),
 * the third "input", InputManagerGlobal's (ADR-0045, the input service), the fourth "content", ContentResolver's
 * (ADR-0045, gap 13a), the fifth "device_identifiers", Build.getSerial's (ADR-0039 step c2d3), the sixth
 * "activity_task", ActivityTaskManager's (ADR-0039 step b3.5b2-1); App's services join it. A name of ABSENT, a
 * service Minato does not have by decision, gives null without the log line.
 */
package android.os;

import android.app.LocalActivityTaskManager;
import android.content.ContentResolver;
import android.content.ContentService;
import android.content.Context;
import android.hardware.display.DisplayService;
import android.hardware.input.InputService;
import android.util.Log;

import com.android.internal.compat.PlatformCompatService;

import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.TreeSet;
import java.util.function.Supplier;

public final class ServiceManager {
    private static final String TAG = "ServiceManager";

    /** The services by name, made at their first lookup; under the class's lock, as the three below. */
    private static final Map<String, Supplier<IBinder>> sMakers = new HashMap<>();
    private static final Map<String, IBinder> sServices = new HashMap<>();
    private static final Set<String> sLoggedUnknown = new HashSet<>();
    /**
     * The services Minato does not have by decision: "network_management", netd's, which StrictMode.setVmPolicy asks
     * at every app's start (step c2d3).
     */
    private static final Set<String> ABSENT = Set.of(Context.NETWORKMANAGEMENT_SERVICE);

    static {
        sMakers.put(Context.DISPLAY_SERVICE, DisplayService::newBinder);
        sMakers.put(Context.PLATFORM_COMPAT_SERVICE, PlatformCompatService::newBinder);
        sMakers.put(Context.INPUT_SERVICE, InputService::newBinder);
        sMakers.put(ContentResolver.CONTENT_SERVICE_NAME, ContentService::newBinder);
        sMakers.put(Context.DEVICE_IDENTIFIERS_SERVICE, DeviceIdentifiersService::newBinder);
        sMakers.put(Context.ACTIVITY_TASK_SERVICE, LocalActivityTaskManager::newBinder);
    }

    private ServiceManager() {
    }

    /** The service of {@code name}, made at its first lookup, or null when the table has none. */
    public static IBinder getService(String name) {
        synchronized (ServiceManager.class) {
            IBinder service = sServices.get(name);
            if (service == null) {
                Supplier<IBinder> maker = sMakers.get(name);
                if (maker == null) {
                    if (!ABSENT.contains(name) && sLoggedUnknown.add(name)) {
                        Log.w(TAG, "No local service \"" + name + "\" (ADR-0045 decision 2)");
                    }
                    return null;
                }
                service = maker.get();
                sServices.put(name, service);
            }
            return service;
        }
    }

    public static IBinder getServiceOrThrow(String name) throws ServiceNotFoundException {
        IBinder service = getService(name);
        if (service == null) {
            throw new ServiceNotFoundException(name);
        }
        return service;
    }

    /** Android's lookup that does not wait for a service to start: the same here, where none starts. */
    public static IBinder checkService(String name) {
        return getService(name);
    }

    /** Puts {@code service} into the table under {@code name}, in place of what was there. */
    public static void addService(String name, IBinder service) {
        synchronized (ServiceManager.class) {
            sMakers.remove(name);
            sServices.put(name, service);
        }
    }

    public static void addService(String name, IBinder service, boolean allowIsolated) {
        addService(name, service);
    }

    public static void addService(String name, IBinder service, boolean allowIsolated, int dumpPriority) {
        addService(name, service);
    }

    /** The names in the table, made or not, in order. */
    public static String[] listServices() {
        synchronized (ServiceManager.class) {
            Set<String> names = new TreeSet<>(sMakers.keySet());
            names.addAll(sServices.keySet());
            return names.toArray(new String[0]);
        }
    }

    /** As AOSP's: what getServiceOrThrow throws for a name the table does not have. */
    public static class ServiceNotFoundException extends Exception {
        private static final long serialVersionUID = 1L;

        public ServiceNotFoundException(String name) {
            super("No service published for: " + name);
        }
    }
}
