// The written part of NsdManager (services bundle C, part 2 of the system services gap): discovery and registration
// of services on the local network (mDNS). Minato has no multicast (COMPATIBILITY.md), so nothing is found and nothing
// is advertised, but an app gets what the emulator (apk-runner-35-bare) answers it [verified: services-i-test]:
// discoverServices reports onDiscoveryStarted, stopServiceDiscovery reports onDiscoveryStopped, registerService
// reports nothing and unregisterService reports onServiceUnregistered. Callbacks come on a thread of the manager's
// own, as AOSP's NsdManager delivers them. The argument checks and the failure on resolve are AOSP's
// [inferred from android-15.0.0_r1, NsdManager.java].
import android.content.Context;
import android.os.Handler;
import android.os.HandlerThread;
import java.util.HashMap;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.Executor;

class NsdManager {
    /** @hide The feature flags that NsdServiceInfo's annotations name (AOSP's NsdManager.Flags). */
    public static class Flags {
        public static final String NSD_SUBTYPES_SUPPORT_ENABLED = "com.android.net.flags.nsd_subtypes_support_enabled";
        public static final String NSD_CUSTOM_HOSTNAME_ENABLED = "com.android.net.flags.nsd_custom_hostname_enabled";
        public static final String NSD_CUSTOM_TTL_ENABLED = "com.android.net.flags.nsd_custom_ttl_enabled";
    }

    private final Map<DiscoveryListener, String> mDiscoveries = new HashMap<>();
    private final Map<RegistrationListener, NsdServiceInfo> mRegistrations = new HashMap<>();
    private Handler mHandler;

    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public NsdManager(Context context) {
    }

    private synchronized Handler handler() {
        if (mHandler == null) {
            final HandlerThread thread = new HandlerThread("NsdManager");
            thread.start();
            mHandler = new Handler(thread.getLooper());
        }
        return mHandler;
    }

    private static void checkProtocol(int protocolType) {
        if (protocolType != PROTOCOL_DNS_SD) throw new IllegalArgumentException("Unsupported protocol");
    }

    public void discoverServices(String serviceType, int protocolType, DiscoveryListener listener) {
        if (serviceType == null || serviceType.isEmpty()) {
            throw new IllegalArgumentException("Service type cannot be empty");
        }
        checkProtocol(protocolType);
        Objects.requireNonNull(listener, "listener cannot be null");
        synchronized (this) {
            if (mDiscoveries.containsKey(listener)) throw new IllegalArgumentException("listener already in use");
            mDiscoveries.put(listener, serviceType);
        }
        handler().post(() -> listener.onDiscoveryStarted(serviceType));
    }

    public void discoverServices(String serviceType, int protocolType, android.net.Network network,
            Executor executor, DiscoveryListener listener) {
        discoverServices(serviceType, protocolType, listener);
    }

    public void stopServiceDiscovery(DiscoveryListener listener) {
        final String type;
        synchronized (this) {
            type = mDiscoveries.remove(listener);
        }
        if (type == null) throw new IllegalArgumentException("listener not registered");
        handler().post(() -> listener.onDiscoveryStopped(type));
    }

    public void registerService(NsdServiceInfo serviceInfo, int protocolType, RegistrationListener listener) {
        Objects.requireNonNull(serviceInfo);
        if (serviceInfo.getPort() <= 0) throw new IllegalArgumentException("Invalid port number");
        checkProtocol(protocolType);
        Objects.requireNonNull(listener, "listener cannot be null");
        synchronized (this) {
            if (mRegistrations.containsKey(listener)) throw new IllegalArgumentException("listener already in use");
            mRegistrations.put(listener, serviceInfo);
        }
    }

    public void registerService(NsdServiceInfo serviceInfo, int protocolType, Executor executor,
            RegistrationListener listener) {
        registerService(serviceInfo, protocolType, listener);
    }

    public void unregisterService(RegistrationListener listener) {
        final NsdServiceInfo info;
        synchronized (this) {
            info = mRegistrations.remove(listener);
        }
        if (info == null) throw new IllegalArgumentException("listener not registered");
        handler().post(() -> listener.onServiceUnregistered(info));
    }

    public void resolveService(NsdServiceInfo serviceInfo, ResolveListener listener) {
        Objects.requireNonNull(serviceInfo);
        Objects.requireNonNull(listener, "listener cannot be null");
        handler().post(() -> listener.onResolveFailed(serviceInfo, FAILURE_INTERNAL_ERROR));
    }

    public void resolveService(NsdServiceInfo serviceInfo, Executor executor, ResolveListener listener) {
        resolveService(serviceInfo, listener);
    }
}
