package com.android.minato.location;

import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;

/**
 * The registrations of the location service (ADR-0063 decisions 3 and 5) on the host's JVM
 * (tools/framework-host-check.sh): what the host is asked for, which fix reaches which listener, and how AOSP's
 * LocationProviderManager throttles: the minimum interval (with its 30 ms of jitter) and the minimum distance, the
 * update count and the duration, the passive listener that gets what the others cause, and single requests with their
 * timeout.
 */
public final class LocationBrokerTest {
    private static final class FakeHost implements LocationBroker.Host {
        final List<String> calls = new ArrayList<>();

        @Override
        public void start(int accuracy, int distanceM) {
            calls.add("start " + accuracy + " " + distanceM);
        }

        @Override
        public void stop() {
            calls.add("stop");
        }

        @Override
        public void requestSingle(long token) {
            calls.add("single " + token);
        }

        @Override
        public void cancelSingle(long token) {
            calls.add("cancel " + token);
        }
    }

    private static final class Got implements LocationBroker.Sink {
        final List<LocationBroker.Fix> fixes = new ArrayList<>();
        int expired;

        @Override
        public void deliver(LocationBroker.Fix fix) {
            fixes.add(fix);
        }

        @Override
        public void expire() {
            expired++;
        }
    }

    private long mNow;
    private final FakeHost mHost = new FakeHost();
    private final LocationBroker mBroker = new LocationBroker(mHost, () -> mNow);

    private static LocationBroker.Fix fix(double lat, double lon, long elapsedMs) {
        LocationBroker.Fix fix = new LocationBroker.Fix();
        fix.latitude = lat;
        fix.longitude = lon;
        fix.elapsedRealtimeNanos = elapsedMs * 1_000_000L;
        return fix;
    }

    private static LocationBroker.Registration reg(String provider, long minIntervalMs, float minDistanceM) {
        return new LocationBroker.Registration(provider, minIntervalMs, minDistanceM, 0, 0, true);
    }

    public void testTheFirstRegistrationStartsTheHostAndTheLastStopsIt() {
        Got got = new Got();
        Object key = mBroker.register(reg("gps", 0, 0), got);
        check(mHost.calls.equals(List.of("start 1 0")), "fine start: " + mHost.calls);
        mBroker.unregister(key);
        check(mHost.calls.equals(List.of("start 1 0", "stop")), "stop: " + mHost.calls);
        mBroker.unregister(key);
        check(mHost.calls.size() == 2, "a second unregister changes nothing");
    }

    public void testTheHostGetsTheSmallestDistanceAndTheBestAccuracy() {
        Object a = mBroker.register(new LocationBroker.Registration("gps", 0, 50, 0, 0, false), new Got());
        check(mHost.calls.get(mHost.calls.size() - 1).equals("start 2 50"), "coarse: " + mHost.calls);
        mBroker.register(new LocationBroker.Registration("fused", 0, 10, 0, 0, true), new Got());
        check(mHost.calls.get(mHost.calls.size() - 1).equals("start 1 10"), "fine and 10 m: " + mHost.calls);
        mBroker.unregister(a);
        check(mHost.calls.get(mHost.calls.size() - 1).equals("start 1 10"), "still fine and 10 m: " + mHost.calls);
    }

    public void testAPassiveListenerStartsNothingAndGetsWhatOthersCause() {
        Got passive = new Got();
        Object key = mBroker.register(reg("passive", 0, 0), passive);
        check(mHost.calls.isEmpty(), "passive alone asks nothing: " + mHost.calls);
        mBroker.onFix(fix(1, 2, 1000));
        check(passive.fixes.isEmpty(), "and gets nothing with no other listener, as nobody asked the host");
        Got gps = new Got();
        mBroker.register(reg("gps", 0, 0), gps);
        mBroker.onFix(fix(1, 2, 2000));
        check(passive.fixes.size() == 1 && gps.fixes.size() == 1, "both get the fix");
        mBroker.unregister(key);
    }

    public void testTheMinimumIntervalThrottlesWithAMarginOf30Ms() {
        Got got = new Got();
        mBroker.register(reg("gps", 1000, 0), got);
        mBroker.onFix(fix(1, 2, 0));
        mBroker.onFix(fix(1, 2, 500));
        mBroker.onFix(fix(1, 2, 969));
        mBroker.onFix(fix(1, 2, 970));
        mBroker.onFix(fix(1, 2, 1900));
        mBroker.onFix(fix(1, 2, 2000));
        check(got.fixes.size() == 3, "0, 970 and 2000 pass, got " + got.fixes.size());
    }

    public void testTheMinimumDistanceThrottles() {
        Got got = new Got();
        mBroker.register(reg("gps", 0, 1000), got);
        mBroker.onFix(fix(52.0, 13.0, 0));
        mBroker.onFix(fix(52.001, 13.0, 1000));   // about 111 m
        mBroker.onFix(fix(52.02, 13.0, 2000));    // about 2.2 km from the first
        check(got.fixes.size() == 2, "the first and the far one: " + got.fixes.size());
    }

    public void testTheMaximumNumberOfUpdatesEndsTheRegistration() {
        Got got = new Got();
        mBroker.register(new LocationBroker.Registration("gps", 0, 0, 2, 0, true), got);
        mBroker.onFix(fix(1, 2, 0));
        mBroker.onFix(fix(1, 2, 10));
        mBroker.onFix(fix(1, 2, 20));
        check(got.fixes.size() == 2 && got.expired == 1,
                "two fixes and one end: " + got.fixes.size() + "/" + got.expired);
        check(mHost.calls.get(mHost.calls.size() - 1).equals("stop"), "and the host stops: " + mHost.calls);
    }

    public void testTheDurationEndsTheRegistration() {
        Got got = new Got();
        mBroker.register(new LocationBroker.Registration("gps", 0, 0, 0, 5000, true), got);
        mNow = 4999;
        mBroker.tick();
        check(got.expired == 0, "not yet");
        mNow = 5000;
        mBroker.tick();
        check(got.expired == 1, "ended at its duration");
        mBroker.onFix(fix(1, 2, 6000));
        check(got.fixes.isEmpty(), "and gets nothing more");
    }

    public void testASingleRequestGetsTheOneShotFixOnce() {
        Got got = new Got();
        long token = mBroker.requestSingle(got, true, 30000);
        check(mHost.calls.equals(List.of("single " + token)), "asked: " + mHost.calls);
        mBroker.onSingle(token, fix(5, 6, 100));
        mBroker.onSingle(token, fix(7, 8, 200));
        check(got.fixes.size() == 1 && got.fixes.get(0).latitude == 5, "once");
    }

    public void testASingleRequestTimesOutWithNull() {
        Got got = new Got();
        long token = mBroker.requestSingle(got, true, 30000);
        mNow = 30000;
        mBroker.tick();
        check(got.fixes.size() == 1 && got.fixes.get(0) == null, "a null at the timeout");
        check(mHost.calls.get(mHost.calls.size() - 1).equals("cancel " + token),
                "and the host is told: " + mHost.calls);
        mBroker.onSingle(token, fix(1, 2, 0));
        check(got.fixes.size() == 1, "a late answer is dropped");
    }

    public void testCancellingASingleRequestTellsTheHostAndAnswersNothing() {
        Got got = new Got();
        long token = mBroker.requestSingle(got, true, 30000);
        mBroker.cancelSingle(token);
        check(mHost.calls.get(mHost.calls.size() - 1).equals("cancel " + token), "told: " + mHost.calls);
        mBroker.onSingle(token, fix(1, 2, 0));
        check(got.fixes.isEmpty(), "nothing is delivered");
    }

    public void testAContinuousFixNeverAnswersASingleRequest() {
        Got got = new Got();
        mBroker.requestSingle(got, true, 30000);
        mBroker.register(reg("gps", 0, 0), new Got());
        mBroker.onFix(fix(1, 2, 0));
        check(got.fixes.isEmpty(), "a single waits for its own fix");
    }

    private static void check(boolean condition, String what) {
        if (!condition) {
            throw new AssertionError(what);
        }
    }

    public static void main(String[] args) throws Exception {
        int failed = 0, run = 0;
        for (Method test : LocationBrokerTest.class.getDeclaredMethods()) {
            if (!test.getName().startsWith("test")) {
                continue;
            }
            run++;
            try {
                test.invoke(new LocationBrokerTest());
            } catch (java.lang.reflect.InvocationTargetException e) {
                failed++;
                System.out.println("FAIL " + test.getName() + ": " + e.getCause());
            }
        }
        System.out.println("LocationBrokerTest: " + (run - failed) + " of " + run + " fine");
        if (failed != 0) {
            System.exit(1);
        }
    }
}
