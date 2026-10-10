package io.github.kisame76.minato.test.location;

import android.app.Application;
import android.location.Location;
import android.location.LocationListener;
import android.os.Bundle;
import android.util.Log;

import java.util.ArrayList;
import java.util.List;

/**
 * Location and LocationListener as an app uses them without a provider (Unity's player names the listener in a class
 * of its own, which its first frame looks up): a location's fields and flags, the great-circle distance and bearing,
 * the coordinate formats and their parsing, and a listener's default methods and its batch call.
 */
public class LocationApplication extends Application {
    static final String TAG = "location-test";

    private static void log(String line) {
        Log.i(TAG, line);
    }

    @Override
    public void onCreate() {
        super.onCreate();
        Location tokyo = new Location("test");
        tokyo.setLatitude(35.6762);
        tokyo.setLongitude(139.6503);
        tokyo.setAltitude(40.5);
        tokyo.setAccuracy(12.5f);
        tokyo.setSpeed(3.25f);
        tokyo.setBearing(90f);
        tokyo.setTime(1700000000000L);
        log("a location: " + tokyo.getProvider() + " " + tokyo.getLatitude() + " " + tokyo.getLongitude() + " "
                + tokyo.getAltitude() + " " + tokyo.getAccuracy() + " " + tokyo.getSpeed() + " " + tokyo.getBearing()
                + " " + tokyo.getTime());
        log("which fields it has: altitude " + tokyo.hasAltitude() + ", accuracy " + tokyo.hasAccuracy() + ", speed "
                + tokyo.hasSpeed() + ", bearing " + tokyo.hasBearing() + ", vertical accuracy "
                + tokyo.hasVerticalAccuracy() + ", mock " + tokyo.isMock());
        tokyo.removeAccuracy();
        tokyo.removeBearing();
        log("after removing two: accuracy " + tokyo.hasAccuracy() + " " + tokyo.getAccuracy() + ", bearing "
                + tokyo.hasBearing() + " " + tokyo.getBearing());
        Location osaka = new Location(tokyo);
        osaka.setLatitude(34.6937);
        osaka.setLongitude(135.5023);
        log("a copy: " + osaka.getProvider() + " " + osaka.getAltitude() + " " + osaka.getTime());
        log("distance and bearing: " + Math.round(tokyo.distanceTo(osaka)) + " m, "
                + Math.round(tokyo.bearingTo(osaka) * 100) / 100.0 + " degrees");
        float[] results = new float[3];
        Location.distanceBetween(35.6762, 139.6503, 34.6937, 135.5023, results);
        log("distanceBetween: " + Math.round(results[0]) + " m, initial " + Math.round(results[1] * 100) / 100.0
                + ", final " + Math.round(results[2] * 100) / 100.0);
        log("formats: " + Location.convert(35.6762, Location.FORMAT_DEGREES) + " | "
                + Location.convert(35.6762, Location.FORMAT_MINUTES) + " | "
                + Location.convert(-139.6503, Location.FORMAT_SECONDS));
        log("parsed: " + Location.convert("35:40:34.32") + " " + Location.convert("-139:39.018"));
        log("a bad coordinate: " + outcome(() -> Location.convert("north")));
        log("a latitude out of range: " + outcome(() -> Location.convert(95.0, 99)));
        Bundle extras = new Bundle();
        extras.putInt("satellites", 7);
        tokyo.setExtras(extras);
        log("extras: " + tokyo.getExtras().getInt("satellites"));
        log("the text: " + osaka.toString().replaceAll("et=[^ ]*", "et=N").replaceAll("t=[^ ]*", "t=N"));

        List<String> calls = new ArrayList<>();
        LocationListener listener = new LocationListener() {
            @Override
            public void onLocationChanged(Location location) {
                calls.add("changed " + location.getLatitude());
            }
        };
        listener.onProviderEnabled("gps");
        listener.onProviderDisabled("gps");
        listener.onStatusChanged("gps", 2, null);
        List<Location> batch = new ArrayList<>();
        batch.add(tokyo);
        batch.add(osaka);
        listener.onLocationChanged(batch);
        listener.onFlushComplete(3);
        log("a listener: " + calls);
        log("done");
    }

    private interface Call {
        Object run() throws Exception;
    }

    private static String outcome(Call call) {
        try {
            return String.valueOf(call.run());
        } catch (Exception e) {
            return e.getClass().getName() + ": " + e.getMessage();
        }
    }
}
