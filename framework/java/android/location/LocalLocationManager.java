/*
 * The location service (ADR-0063): ILocationManager of system_server, local in the app's process, which
 * ServiceManager's table gives as "location" and AOSP's LocationManager asks. The host's CoreLocation is behind the
 * runtime's location hub (APKRT_NR_LOCATION), which gates every call on the grant Minato's own sheet recorded and
 * makes a coarse grant's fixes coarse; this class keeps what Android keeps per listener (LocationBroker: the
 * throttle, the update count, the duration, one fix per getCurrentLocation) and answers as Android 15's
 * LocationManagerService does for an app [measured: location-manager-test and its twins on apk-runner-35]:
 *   - the providers are fused, gps and passive; "network" does not exist, so a request for it and a query of its
 *     properties are an IllegalArgumentException, and hasProvider and isProviderEnabledForUser answer false;
 *   - without ACCESS_COARSE_LOCATION or ACCESS_FINE_LOCATION a request for fixes, the last fix or a current location
 *     is a SecurityException with Android's text, the providers that are enabled are none, and the GNSS callbacks need
 *     the fine permission;
 *   - an app with the coarse permission only gets the host's coarse fixes from any provider;
 *   - the fixes of a listener come on the main thread's looper through the app's LocationListenerTransport.
 * Not here: PendingIntent updates, geofences, geocoding, GNSS measurements and status, batching and test providers,
 * which answer with AIDL's Default. Not under com.android.minato, whose classes tools/framework-host-check.sh
 * compiles on the host without android.*.
 */
package android.location;

import android.app.ActivityThread;
import android.location.provider.ProviderProperties;
import android.os.Binder;
import android.os.Handler;
import android.os.IBinder;
import android.os.ICancellationSignal;
import android.os.Looper;
import android.os.Process;
import android.os.RemoteException;
import android.os.SystemClock;
import android.util.Log;

import com.android.minato.location.LocationBroker;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

public final class LocalLocationManager extends ILocationManager.Default implements LocationBroker.Host {
    private static final String TAG = "LocalLocationManager";
    private static final String FINE = "android.permission.ACCESS_FINE_LOCATION";
    private static final String COARSE = "android.permission.ACCESS_COARSE_LOCATION";
    /** The order of Android 15's getAllProviders. */
    private static final List<String> PROVIDERS =
            Collections.unmodifiableList(Arrays.asList("fused", "gps", "passive"));
    private static final int EVENT_ONE_SHOT = 1;
    private static final int EVENT_COARSE = 2;
    private static final int EVENT_NO_FIX = 4;
    /** How often what has run out is looked for: a listener's duration, a current location's timeout. */
    private static final long TICK_MILLIS = 250;
    /** getCurrentLocation's longest wait when the request names none. */
    private static final long CURRENT_TIMEOUT_MILLIS = 30_000;

    private static volatile LocalLocationManager sInstance;

    private final Binder mBinder = new Binder();
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private final LocationBroker mBroker = new LocationBroker(this, SystemClock::elapsedRealtime);
    /** The app's listeners by their binder, and the broker's key for each; the current locations not yet answered. */
    private final Map<IBinder, Object> mKeys = new HashMap<>();
    private int mSingles;
    private boolean mTicking;
    /** Whether the app asked for fixes once: Android's providers run only then, so before it nothing is known. */
    private volatile boolean mRequested;

    private LocalLocationManager() {
        mBinder.attachInterface(this, ILocationManager.Stub.DESCRIPTOR);
        sInstance = this;
    }

    /** ServiceManager's maker for "location": the Binder that carries a new service. */
    public static IBinder newBinder() {
        return new LocalLocationManager().asBinder();
    }

    @Override
    public IBinder asBinder() {
        return mBinder;
    }

    // MARK: - The host

    @Override
    public void start(int accuracy, int distanceM) {
        final int error = ActivityThread.locationStart(accuracy, distanceM);
        if (error != 0) {
            Log.w(TAG, "The host refused the location updates: " + error);
        }
    }

    @Override
    public void stop() {
        ActivityThread.locationStop();
    }

    @Override
    public void requestSingle(long token) {
        // A request the host refuses is answered by the broker's timeout, with no location.
        final int error = ActivityThread.locationCurrent(token);
        if (error != 0) {
            Log.w(TAG, "The host refused the current location: " + error);
        }
    }

    @Override
    public void cancelSingle(long token) {
        ActivityThread.locationCancel(token);
    }

    /** APKRT_EVENT_LOCATION on the main thread: a fix to the listeners, or a one-shot's answer to its callback. */
    public static void onEvent(long sequence, int flags, long token) {
        final LocalLocationManager service = sInstance;
        if (service == null) {
            return;
        }
        if ((flags & EVENT_NO_FIX) != 0) {
            service.mBroker.onSingle(token, null);
            return;
        }
        final double[] doubles = new double[9];
        final long[] longs = new long[4];
        if (!ActivityThread.locationGet(sequence, doubles, longs)) {
            return;
        }
        final LocationBroker.Fix fix = fix(doubles, longs);
        fix.reduced = (flags & EVENT_COARSE) != 0;
        if ((flags & EVENT_ONE_SHOT) != 0) {
            service.mBroker.onSingle(token, fix);
        } else {
            service.mBroker.onFix(fix);
        }
    }

    private static LocationBroker.Fix fix(double[] d, long[] l) {
        final LocationBroker.Fix fix = new LocationBroker.Fix();
        fix.latitude = d[0];
        fix.longitude = d[1];
        fix.altitude = d[2];
        fix.horizontalAccuracy = (float) d[3];
        fix.verticalAccuracy = (float) d[4];
        fix.speed = (float) d[5];
        fix.speedAccuracy = (float) d[6];
        fix.bearing = (float) d[7];
        fix.bearingAccuracy = (float) d[8];
        fix.timeMillis = l[0];
        fix.elapsedRealtimeNanos = l[1];
        fix.valid = (int) l[3];
        return fix;
    }

    /** The Location of a fix for a provider name; the optional fields only when the host had them. */
    private static Location location(String provider, LocationBroker.Fix fix) {
        final Location location = new Location(provider);
        location.setLatitude(fix.latitude);
        location.setLongitude(fix.longitude);
        location.setTime(fix.timeMillis);
        location.setElapsedRealtimeNanos(fix.elapsedRealtimeNanos);
        if ((fix.valid & 1) != 0) {
            location.setAltitude(fix.altitude);
        }
        if ((fix.valid & 2) != 0) {
            location.setAccuracy(fix.horizontalAccuracy);
        }
        if ((fix.valid & 4) != 0) {
            location.setVerticalAccuracyMeters(fix.verticalAccuracy);
        }
        if ((fix.valid & 8) != 0) {
            location.setSpeed(fix.speed);
        }
        if ((fix.valid & 16) != 0) {
            location.setSpeedAccuracyMetersPerSecond(fix.speedAccuracy);
        }
        if ((fix.valid & 32) != 0) {
            location.setBearing(fix.bearing);
        }
        if ((fix.valid & 64) != 0) {
            location.setBearingAccuracyDegrees(fix.bearingAccuracy);
        }
        return location;
    }

    /** The provider name a fix carries for a listener: its own, and for the passive provider the source's, the GPS. */
    private static String nameOf(String provider) {
        return "passive".equals(provider) ? "gps" : provider;
    }

    // MARK: - Permissions

    private static boolean fine() {
        return ActivityThread.permissionGranted(FINE);
    }

    private static boolean coarse() {
        return ActivityThread.permissionGranted(COARSE);
    }

    private static void enforceLocation() {
        if (!fine() && !coarse()) {
            throw new SecurityException("uid " + Process.myUid() + " does not have " + COARSE + " or " + FINE + ".");
        }
    }

    private static void enforceFine() {
        if (!fine()) {
            throw new SecurityException("Neither user " + Process.myUid() + " nor current process has " + FINE + ".");
        }
    }

    private static void enforceProvider(String provider) {
        if (!PROVIDERS.contains(provider)) {
            throw new IllegalArgumentException("provider \"" + provider + "\" does not exist");
        }
    }

    // MARK: - Providers

    @Override
    public boolean hasProvider(String provider) {
        return PROVIDERS.contains(provider);
    }

    @Override
    public List<String> getAllProviders() {
        return new ArrayList<>(PROVIDERS);
    }

    @Override
    @SuppressWarnings("deprecation")
    public List<String> getProviders(Criteria criteria, boolean enabledOnly) {
        // Android lists the enabled ones for an app that may see locations only.
        return enabledOnly && !fine() && !coarse() ? new ArrayList<>() : new ArrayList<>(PROVIDERS);
    }

    @Override
    @SuppressWarnings("deprecation")
    public String getBestProvider(Criteria criteria, boolean enabledOnly) {
        return enabledOnly && !fine() && !coarse() ? null : "gps";
    }

    @Override
    public boolean isProviderEnabledForUser(String provider, int userId) {
        return PROVIDERS.contains(provider);
    }

    @Override
    public boolean isLocationEnabledForUser(int userId) {
        return true;
    }

    @Override
    public ProviderProperties getProviderProperties(String provider) {
        // LocationManager.getProvider reads a missing provider from this exception.
        enforceProvider(provider);
        final boolean passive = "passive".equals(provider);
        return new ProviderProperties.Builder()
                .setHasNetworkRequirement(false)
                .setHasSatelliteRequirement("gps".equals(provider))
                .setHasCellRequirement(false)
                .setHasMonetaryCost(false)
                .setHasAltitudeSupport(true)
                .setHasSpeedSupport(true)
                .setHasBearingSupport(true)
                .setPowerUsage(passive ? ProviderProperties.POWER_USAGE_LOW : ProviderProperties.POWER_USAGE_HIGH)
                .setAccuracy(passive ? ProviderProperties.ACCURACY_COARSE : ProviderProperties.ACCURACY_FINE)
                .build();
    }

    // MARK: - Fixes

    @Override
    public Location getLastLocation(String provider, LastLocationRequest request, String packageName,
            String attributionTag) {
        enforceLocation();
        if (!PROVIDERS.contains(provider) || !mRequested) {
            return null;
        }
        final double[] doubles = new double[9];
        final long[] longs = new long[4];
        return ActivityThread.locationLast(doubles, longs) ? location(nameOf(provider), fix(doubles, longs)) : null;
    }

    /** A listener's sink: the fix as a Location in a list for the transport, and the end of a registration. */
    private final class ListenerSink implements LocationBroker.Sink {
        private final ILocationListener mListener;
        private final String mProvider;
        private final IBinder mBinderKey;

        ListenerSink(ILocationListener listener, String provider) {
            mListener = listener;
            mProvider = provider;
            mBinderKey = listener.asBinder();
        }

        @Override
        public void deliver(LocationBroker.Fix fix) {
            mHandler.post(() -> {
                try {
                    mListener.onLocationChanged(Collections.singletonList(location(nameOf(mProvider), fix)), null);
                } catch (RemoteException e) {
                    Log.w(TAG, "A listener's delivery failed", e);
                }
            });
        }

        @Override
        public void expire() {
            synchronized (mKeys) {
                mKeys.remove(mBinderKey);
            }
        }
    }

    @Override
    public void registerLocationListener(String provider, LocationRequest request, ILocationListener listener,
            String packageName, String attributionTag, String listenerId) {
        enforceLocation();
        enforceProvider(provider);
        final long duration = request.getDurationMillis();
        final int maxUpdates = request.getMaxUpdates();
        final LocationBroker.Registration registration = new LocationBroker.Registration(provider,
                request.getMinUpdateIntervalMillis(), request.getMinUpdateDistanceMeters(),
                maxUpdates == Integer.MAX_VALUE ? 0 : maxUpdates, duration >= Long.MAX_VALUE / 2 ? 0 : duration,
                fine());
        mRequested = true;
        final IBinder binder = listener.asBinder();
        final Object old;
        synchronized (mKeys) {
            old = mKeys.remove(binder);
        }
        if (old != null) {
            mBroker.unregister(old);
        }
        final Object key = mBroker.register(registration, new ListenerSink(listener, provider));
        synchronized (mKeys) {
            mKeys.put(binder, key);
        }
        startTicking();
    }

    @Override
    public void unregisterLocationListener(ILocationListener listener) {
        final Object key;
        synchronized (mKeys) {
            key = mKeys.remove(listener.asBinder());
        }
        if (key != null) {
            mBroker.unregister(key);
        }
    }

    @Override
    public ICancellationSignal getCurrentLocation(String provider, LocationRequest request, ILocationCallback callback,
            String packageName, String attributionTag, String listenerId) {
        enforceLocation();
        enforceProvider(provider);
        final long timeout = request.getDurationMillis();
        mRequested = true;
        synchronized (mKeys) {
            mSingles++;
        }
        final long token = mBroker.requestSingle(new LocationBroker.Sink() {
            @Override
            public void deliver(LocationBroker.Fix fix) {
                synchronized (mKeys) {
                    mSingles--;
                }
                mHandler.post(() -> {
                    try {
                        callback.onLocation(fix == null ? null : location(nameOf(provider), fix));
                    } catch (RemoteException e) {
                        Log.w(TAG, "A current location's delivery failed", e);
                    }
                });
            }

            @Override
            public void expire() {
            }
        }, fine(), timeout >= Long.MAX_VALUE / 2 ? CURRENT_TIMEOUT_MILLIS : timeout);
        startTicking();
        return new ICancellationSignal.Stub() {
            @Override
            public void cancel() {
                // The broker forgets a cancelled request without an answer, so the count ends here.
                if (mBroker.cancelSingle(token)) {
                    synchronized (mKeys) {
                        mSingles--;
                    }
                }
            }
        };
    }

    /** Looks every TICK_MILLIS for what has run out while a listener or a current location is kept. */
    private void startTicking() {
        synchronized (mKeys) {
            if (mTicking) {
                return;
            }
            mTicking = true;
        }
        mHandler.postDelayed(this::tick, TICK_MILLIS);
    }

    private void tick() {
        mBroker.tick();
        synchronized (mKeys) {
            if (mKeys.isEmpty() && mSingles <= 0) {
                mTicking = false;
                return;
            }
        }
        mHandler.postDelayed(this::tick, TICK_MILLIS);
    }

    // MARK: - GNSS

    @Override
    public void registerGnssStatusCallback(IGnssStatusListener callback, String packageName, String attributionTag,
            String listenerId) {
        enforceFine();
    }

    @Override
    public void registerGnssNmeaCallback(IGnssNmeaListener callback, String packageName, String attributionTag,
            String listenerId) {
        enforceFine();
    }
}
