package com.android.minato.location;

import java.util.ArrayList;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.function.LongSupplier;

/**
 * The registrations of the location service (ADR-0063 decisions 3 and 5): which of the app's listeners get which fix
 * of the host, and what the host is asked for. The host is told the wanted state as a whole, the best accuracy of the
 * listeners and the smallest distance (it owns no per-listener state), and the throttling CoreLocation lacks is done
 * here as AOSP's LocationProviderManager does it: a fix reaches a listener only when its minimum interval (less
 * MAX_FASTEST_INTERVAL_JITTER_MS) and its minimum distance have passed since the last one it got, at most
 * `maxUpdates` times and for at most `duration`. A passive listener asks nothing and gets every fix the others cause.
 * A single request is answered by its own fix, once, or by null at its timeout, and a continuous fix never answers it.
 * Plain Java, so tools/framework-host-check.sh runs it on the host's JVM; the sinks are called outside the lock.
 */
public final class LocationBroker {
    /** The accuracy the host is asked for; the values of the runtime's APKRT_LOCATION_ACCURACY_*. */
    public static final int ACCURACY_FINE = 1;
    public static final int ACCURACY_COARSE = 2;
    /** AOSP's allowance for a fix that comes slightly early (LocationProviderManager). */
    private static final long MAX_FASTEST_INTERVAL_JITTER_MS = 30;
    private static final double EARTH_RADIUS_M = 6371000;

    /** What the host does, record-only and quick. */
    public interface Host {
        void start(int accuracy, int distanceM);

        void stop();

        void requestSingle(long token);

        void cancelSingle(long token);
    }

    /**
     * Where a listener's fixes go; `expire` ends it (the update count or the duration). A null fix is a single's
     * timeout.
     */
    public interface Sink {
        void deliver(Fix fix);

        void expire();
    }

    /** A fix as the host gave it; the fields of Location. */
    public static final class Fix {
        public double latitude, longitude, altitude;
        public float horizontalAccuracy, verticalAccuracy, speed, speedAccuracy, bearing, bearingAccuracy;
        public long timeMillis, elapsedRealtimeNanos;
        /** Which of the optional fields it has: the runtime's has-bits. */
        public int valid;
        public boolean reduced;
    }

    /** One listener's request. `provider` is "gps", "network", "fused" or "passive". */
    public static final class Registration {
        final String provider;
        final long minIntervalMillis;
        final float minDistanceMeters;
        final int maxUpdates;
        final long durationMillis;
        final boolean fine;

        /**
         * @param maxUpdates     0 for no limit
         * @param durationMillis 0 for no limit
         * @param fine           the app holds the fine permission, so the host is asked for the best accuracy
         */
        public Registration(String provider, long minIntervalMillis, float minDistanceMeters, int maxUpdates,
                            long durationMillis, boolean fine) {
            this.provider = provider;
            this.minIntervalMillis = minIntervalMillis;
            this.minDistanceMeters = minDistanceMeters;
            this.maxUpdates = maxUpdates;
            this.durationMillis = durationMillis;
            this.fine = fine;
        }
    }

    private static final class Entry {
        final Registration registration;
        final Sink sink;
        final long expiresAt;
        int delivered;
        Fix last;

        Entry(Registration registration, Sink sink, long expiresAt) {
            this.registration = registration;
            this.sink = sink;
            this.expiresAt = expiresAt;
        }

        boolean passive() {
            return "passive".equals(registration.provider);
        }
    }

    private static final class Single {
        final Sink sink;
        final long deadline;

        Single(Sink sink, long deadline) {
            this.sink = sink;
            this.deadline = deadline;
        }
    }

    private final Host mHost;
    private final LongSupplier mNowMillis;
    private final Map<Object, Entry> mEntries = new LinkedHashMap<>();
    private final Map<Long, Single> mSingles = new LinkedHashMap<>();
    private long mNextToken = 1;
    private boolean mStarted;
    private int mStartedAccuracy;
    private int mStartedDistance;

    /** @param nowMillis the clock durations and timeouts run on */
    public LocationBroker(Host host, LongSupplier nowMillis) {
        mHost = host;
        mNowMillis = nowMillis;
    }

    /** Adds a listener; the returned key removes it. */
    public Object register(Registration registration, Sink sink) {
        Object key = new Object();
        synchronized (this) {
            long expiresAt = registration.durationMillis > 0 ? mNowMillis.getAsLong() + registration.durationMillis : 0;
            mEntries.put(key, new Entry(registration, sink, expiresAt));
            reconfigure();
        }
        return key;
    }

    public void unregister(Object key) {
        synchronized (this) {
            if (mEntries.remove(key) != null) {
                reconfigure();
            }
        }
    }

    /** A continuous fix of the host: to every listener whose interval and distance have passed. */
    public void onFix(Fix fix) {
        List<Runnable> deliveries = new ArrayList<>();
        synchronized (this) {
            if (!mStarted) {
                return;
            }
            Iterator<Entry> entries = mEntries.values().iterator();
            boolean ended = false;
            while (entries.hasNext()) {
                Entry entry = entries.next();
                if (!passes(entry, fix)) {
                    continue;
                }
                entry.last = fix;
                entry.delivered++;
                Sink sink = entry.sink;
                deliveries.add(() -> sink.deliver(fix));
                if (entry.registration.maxUpdates > 0 && entry.delivered >= entry.registration.maxUpdates) {
                    entries.remove();
                    deliveries.add(sink::expire);
                    ended = true;
                }
            }
            if (ended) {
                reconfigure();
            }
        }
        deliveries.forEach(Runnable::run);
    }

    private static boolean passes(Entry entry, Fix fix) {
        if (entry.last == null) {
            return true;
        }
        long sinceMillis = (fix.elapsedRealtimeNanos - entry.last.elapsedRealtimeNanos) / 1_000_000L;
        if (sinceMillis < entry.registration.minIntervalMillis - MAX_FASTEST_INTERVAL_JITTER_MS) {
            return false;
        }
        return entry.registration.minDistanceMeters <= 0
                || distanceMeters(entry.last, fix) >= entry.registration.minDistanceMeters;
    }

    /** The great-circle distance, within half a percent of Location.distanceBetween's, which the throttle needs. */
    static double distanceMeters(Fix a, Fix b) {
        double lat1 = Math.toRadians(a.latitude);
        double lat2 = Math.toRadians(b.latitude);
        double dLat = lat2 - lat1;
        double dLon = Math.toRadians(b.longitude - a.longitude);
        double h = Math.sin(dLat / 2) * Math.sin(dLat / 2)
                + Math.cos(lat1) * Math.cos(lat2) * Math.sin(dLon / 2) * Math.sin(dLon / 2);
        return 2 * EARTH_RADIUS_M * Math.asin(Math.min(1, Math.sqrt(h)));
    }

    /** Asks the host for one fix; the token names the request. */
    public long requestSingle(Sink sink, boolean fine, long timeoutMillis) {
        synchronized (this) {
            long token = mNextToken++;
            mSingles.put(token, new Single(sink, mNowMillis.getAsLong() + timeoutMillis));
            mHost.requestSingle(token);
            return token;
        }
    }

    /** The host's answer to a single request: delivered once; an answer to a request that is gone is dropped. */
    public void onSingle(long token, Fix fix) {
        Single single;
        synchronized (this) {
            single = mSingles.remove(token);
        }
        if (single != null) {
            single.sink.deliver(fix);
        }
    }

    /** The app gave the request up: the host is told and nothing is delivered; false when it was not waiting. */
    public boolean cancelSingle(long token) {
        synchronized (this) {
            if (mSingles.remove(token) != null) {
                mHost.cancelSingle(token);
                return true;
            }
            return false;
        }
    }

    /** Ends what has run out: the listeners at their duration, the singles at their timeout with a null. */
    public void tick() {
        List<Runnable> deliveries = new ArrayList<>();
        synchronized (this) {
            long now = mNowMillis.getAsLong();
            boolean ended = false;
            Iterator<Entry> entries = mEntries.values().iterator();
            while (entries.hasNext()) {
                Entry entry = entries.next();
                if (entry.expiresAt > 0 && now >= entry.expiresAt) {
                    entries.remove();
                    deliveries.add(entry.sink::expire);
                    ended = true;
                }
            }
            Iterator<Map.Entry<Long, Single>> singles = mSingles.entrySet().iterator();
            while (singles.hasNext()) {
                Map.Entry<Long, Single> single = singles.next();
                if (now >= single.getValue().deadline) {
                    singles.remove();
                    mHost.cancelSingle(single.getKey());
                    Sink sink = single.getValue().sink;
                    deliveries.add(() -> sink.deliver(null));
                }
            }
            if (ended) {
                reconfigure();
            }
        }
        deliveries.forEach(Runnable::run);
    }

    /** Tells the host the wanted state when it changed; under the lock. */
    private void reconfigure() {
        boolean any = false;
        boolean fine = false;
        float distance = Float.MAX_VALUE;
        for (Entry entry : mEntries.values()) {
            if (entry.passive()) {
                continue;
            }
            any = true;
            fine |= entry.registration.fine;
            distance = Math.min(distance, entry.registration.minDistanceMeters);
        }
        if (!any) {
            if (mStarted) {
                mStarted = false;
                mHost.stop();
            }
            return;
        }
        int accuracy = fine ? ACCURACY_FINE : ACCURACY_COARSE;
        int distanceM = (int) Math.max(0, Math.floor(distance));
        if (!mStarted || accuracy != mStartedAccuracy || distanceM != mStartedDistance) {
            mStarted = true;
            mStartedAccuracy = accuracy;
            mStartedDistance = distanceM;
            mHost.start(accuracy, distanceM);
        }
    }
}
