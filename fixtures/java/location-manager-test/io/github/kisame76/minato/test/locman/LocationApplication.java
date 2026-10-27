package io.github.kisame76.minato.test.locman;

import android.Manifest;
import android.app.Application;
import android.content.pm.PackageManager;
import android.location.GnssStatus;
import android.location.Location;
import android.location.LocationListener;
import android.location.LocationManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.Executor;
import java.util.function.Consumer;

/**
 * LocationManager as an app meets it (ADR-0063): the answers of an app that does not request a location permission,
 * of one that requests and is not granted it, of one granted both and of one granted the coarse one only, from the
 * same sources (the twins location-manager-test, -denied, -fine and -coarse differ in their manifests and in what the
 * harness grants). What the manager answers without the permission (a SecurityException's text), the providers and
 * their enabled state, the last known location, the updates a listener gets and getCurrentLocation's answer are
 * logged under the app's own tag, which is its label. The position the harness set (the emulator's geo fix, Minato's
 * fake host) is compared by its distance and its accuracy class, not its digits: a fine fix lies within 500 m, a
 * coarse one within 3000 m with the accuracy of exactly 2000 m (Android 15 snaps it to cells of about 2 km, measured on
 * apk-runner-35); what depends on the device goes under the tag with -device.
 */
public class LocationApplication extends Application {
    private static final double SET_LAT = 52.52;
    private static final double SET_LON = 13.405;
    private static final String FINE = Manifest.permission.ACCESS_FINE_LOCATION;
    private static final String COARSE = Manifest.permission.ACCESS_COARSE_LOCATION;

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private String mTag;
    private LocationManager mManager;

    private void log(String line) {
        Log.i(mTag, line);
    }

    private void device(String line) {
        Log.i(mTag + "-device", line);
    }

    @Override
    public void onCreate() {
        super.onCreate();
        mTag = String.valueOf(getApplicationInfo().loadLabel(getPackageManager()));
        mManager = (LocationManager) getSystemService(LOCATION_SERVICE);
        log("manager: " + (mManager == null ? "null" : mManager.getClass().getName()));
        if (mManager == null) {
            log("done");
            return;
        }
        log("permissions: fine " + checkSelfPermission(FINE) + ", coarse " + checkSelfPermission(COARSE));
        log("check: " + getPackageManager().checkPermission(FINE, getPackageName()) + " "
                + getPackageManager().checkPermission(COARSE, getPackageName()));
        mHandler.post(this::steps);
    }

    private static String problem(Throwable t) {
        // The uid differs per device and per install; the wording around it is what an app can match on.
        return t.getClass().getName() + ": " + t.getMessage().replaceAll("(uid|user) \\d+", "$1 UID");
    }

    private void steps() {
        List<String> all = new ArrayList<>(mManager.getAllProviders());
        Collections.sort(all);
        log("all providers: " + all);
        List<String> enabled = new ArrayList<>(mManager.getProviders(true));
        Collections.sort(enabled);
        log("enabled providers: " + enabled);
        log("location enabled: " + mManager.isLocationEnabled());
        for (String provider : new String[] {LocationManager.GPS_PROVIDER, LocationManager.NETWORK_PROVIDER,
                LocationManager.PASSIVE_PROVIDER, LocationManager.FUSED_PROVIDER, "nonsense"}) {
            String enabledNow;
            try {
                enabledNow = String.valueOf(mManager.isProviderEnabled(provider));
            } catch (RuntimeException e) {
                enabledNow = problem(e);
            }
            log("provider " + provider + ": has " + mManager.hasProvider(provider) + ", enabled " + enabledNow);
            try {
                log("provider " + provider + ": properties " + (mManager.getProvider(provider) != null));
            } catch (RuntimeException e) {
                log("provider " + provider + ": properties " + problem(e));
            }
        }
        for (String provider : new String[] {LocationManager.GPS_PROVIDER, LocationManager.NETWORK_PROVIDER,
                LocationManager.PASSIVE_PROVIDER}) {
            try {
                Location last = mManager.getLastKnownLocation(provider);
                log("last known " + provider + ": " + (last == null ? "none" : "a location"));
                if (last != null) {
                    device("last known " + provider + ": " + last);
                }
            } catch (RuntimeException e) {
                log("last known " + provider + ": " + problem(e));
            }
        }
        mHandler.post(this::updates);
    }

    /** What a location says about itself, against the position the harness set. */
    private String describe(Location location) {
        float[] result = new float[1];
        Location.distanceBetween(SET_LAT, SET_LON, location.getLatitude(), location.getLongitude(), result);
        return "provider " + location.getProvider() + ", within 500 m " + (result[0] < 500) + ", within 3000 m "
                + (result[0] < 3000) + ", has accuracy " + location.hasAccuracy() + ", accuracy under 100 m "
                + (location.getAccuracy() < 100) + ", accuracy 2000 m " + (location.getAccuracy() == 2000f)
                + ", time fresh " + (Math.abs(System.currentTimeMillis() - location.getTime()) < 60000)
                + ", elapsed realtime fresh "
                + (Math.abs(android.os.SystemClock.elapsedRealtimeNanos() - location.getElapsedRealtimeNanos())
                        < 60_000_000_000L)
                + ", mock " + location.isMock();
    }

    private void updates() {
        final List<Location> got = new ArrayList<>();
        final Thread[] thread = new Thread[1];
        LocationListener listener = new LocationListener() {
            @Override
            public void onLocationChanged(Location location) {
                got.add(location);
                if (thread[0] == null) {
                    thread[0] = Thread.currentThread();
                }
            }
        };
        try {
            mManager.requestLocationUpdates(LocationManager.GPS_PROVIDER, 0, 0, listener);
            log("updates gps: requested");
        } catch (RuntimeException e) {
            log("updates gps: " + problem(e));
            mHandler.post(this::current);
            return;
        }
        mHandler.postDelayed(() -> {
            mManager.removeUpdates(listener);
            log("updates gps: got a location " + !got.isEmpty());
            if (!got.isEmpty()) {
                log("updates gps: first, " + describe(got.get(0)));
                log("updates gps: on the main thread " + (thread[0] == Looper.getMainLooper().getThread()));
            }
            final int before = got.size();
            mHandler.postDelayed(() -> {
                log("updates gps: after removing " + (got.size() - before));
                current();
            }, 700);
        }, 4000);
    }

    private void current() {
        Executor executor = mHandler::post;
        final boolean[] answered = new boolean[1];
        Consumer<Location> consumer = location -> {
            answered[0] = true;
            log("current gps: " + (location == null ? "null" : describe(location)));
        };
        try {
            mManager.getCurrentLocation(LocationManager.GPS_PROVIDER, null, executor, consumer);
            log("current gps: requested");
        } catch (RuntimeException e) {
            log("current gps: " + problem(e));
            mHandler.post(this::gnss);
            return;
        }
        mHandler.postDelayed(() -> {
            log("current gps: answered " + answered[0]);
            gnss();
        }, 4000);
    }

    private void gnss() {
        GnssStatus.Callback callback = new GnssStatus.Callback() { };
        try {
            log("gnss: registered " + mManager.registerGnssStatusCallback(callback, mHandler));
            mManager.unregisterGnssStatusCallback(callback);
        } catch (RuntimeException e) {
            log("gnss: " + problem(e));
        }
        log("done");
    }
}
