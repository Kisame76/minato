package io.github.kisame76.minato.test.sensorsjava;

import android.app.Application;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;

import java.util.ArrayList;
import java.util.List;

/**
 * SensorManager over the sensors that CoreMotion feeds (ADR-0061 step 2), as an app meets it: which of the offered
 * types exist and their fixed properties; for each, a listener at SENSOR_DELAY_GAME gets events whose shape is checked
 * (values, accuracy, timestamps), and after unregistering none come. What depends on the device (names, vendors, power,
 * resolution, range, the exact rates, and the types an emulator lacks) goes under the device tag. The listener runs on
 * the main looper.
 */
public class SensorsApplication extends Application {
    static final String TAG = "sensors-java-test";
    static final String DEVICE_TAG = "sensors-java-test-device";

    /** The types ADR-0061 offers, with the number of values an event carries. */
    private static final int[] TYPES = {Sensor.TYPE_ACCELEROMETER, Sensor.TYPE_GYROSCOPE, Sensor.TYPE_MAGNETIC_FIELD,
        Sensor.TYPE_GRAVITY, Sensor.TYPE_LINEAR_ACCELERATION, Sensor.TYPE_ROTATION_VECTOR,
        Sensor.TYPE_GAME_ROTATION_VECTOR, Sensor.TYPE_MAGNETIC_FIELD_UNCALIBRATED, Sensor.TYPE_GYROSCOPE_UNCALIBRATED};
    private static final int[] VALUES = {3, 3, 3, 3, 3, 4, 4, 6, 6};

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private SensorManager mManager;
    private int mIndex;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    @Override
    public void onCreate() {
        super.onCreate();
        mManager = (SensorManager) getSystemService(SENSOR_SERVICE);
        log("manager: " + mManager.getClass().getName());
        for (int type : TYPES) {
            Sensor sensor = mManager.getDefaultSensor(type);
            Log.i(DEVICE_TAG, "type " + type + ": " + (sensor == null ? "absent" : sensor.getName() + " by "
                    + sensor.getVendor() + " v" + sensor.getVersion() + " power " + sensor.getPower() + " resolution "
                    + sensor.getResolution() + " range " + sensor.getMaximumRange() + " minDelay "
                    + sensor.getMinDelay() + " maxDelay " + sensor.getMaxDelay()));
        }
        Log.i(DEVICE_TAG, "all sensors: " + mManager.getSensorList(Sensor.TYPE_ALL).size());
        log("absent type 9999: " + (mManager.getDefaultSensor(9999) == null) + ", list "
                + mManager.getSensorList(9999).size());
        log("register null sensor: " + mManager.registerListener(new Quiet(), (Sensor) null,
                SensorManager.SENSOR_DELAY_GAME));
        mHandler.post(this::next);
    }

    /** A listener that does nothing. */
    private static final class Quiet implements SensorEventListener {
        @Override
        public void onSensorChanged(SensorEvent event) {
        }

        @Override
        public void onAccuracyChanged(Sensor sensor, int accuracy) {
        }
    }

    /** Collects the events of one sensor and judges them. */
    private final class Collector implements SensorEventListener {
        final Sensor sensor;
        final int values;
        final List<SensorEvent> events = new ArrayList<>();
        final List<float[]> copies = new ArrayList<>();
        final List<Long> stamps = new ArrayList<>();
        final List<Long> arrivals = new ArrayList<>();
        boolean sameSensor = true;
        boolean wrongThread;

        Collector(Sensor sensor, int values) {
            this.sensor = sensor;
            this.values = values;
        }

        @Override
        public void onSensorChanged(SensorEvent event) {
            wrongThread |= Looper.myLooper() != Looper.getMainLooper();
            sameSensor &= event.sensor == sensor;
            copies.add(event.values.clone());
            stamps.add(event.timestamp);
            arrivals.add(SystemClock.elapsedRealtimeNanos());
            events.add(event);
            if (event.accuracy < 0 || event.accuracy > 3) {
                copies.add(null);
            }
        }

        @Override
        public void onAccuracyChanged(Sensor changed, int accuracy) {
        }
    }

    private void next() {
        if (mIndex == TYPES.length) {
            log("done");
            return;
        }
        final int type = TYPES[mIndex];
        final int values = VALUES[mIndex];
        mIndex++;
        final Sensor sensor = mManager.getDefaultSensor(type);
        if (sensor == null) {
            Log.i(DEVICE_TAG, "type " + type + ": not tested, absent");
            mHandler.post(this::next);
            return;
        }
        final Collector collector = new Collector(sensor, values);
        log("type " + type + ": stringType " + sensor.getStringType() + ", reporting mode "
                + sensor.getReportingMode() + ", wake-up " + sensor.isWakeUpSensor() + ", dynamic "
                + sensor.isDynamicSensor() + ", fifoMax " + sensor.getFifoMaxEventCount() + ", minDelay>0 "
                + (sensor.getMinDelay() > 0));
        // The emulator's composite sensors have a maximum delay of 0, a real device's of 1 s or more.
        Log.i(DEVICE_TAG, "type " + type + ": maxDelay>=minDelay "
                + (sensor.getMaxDelay() >= sensor.getMinDelay()));
        log("type " + type + ": register " + mManager.registerListener(collector, sensor,
                SensorManager.SENSOR_DELAY_GAME));
        mHandler.postDelayed(() -> {
            mManager.unregisterListener(collector, sensor);
            final int before = collector.events.size();
            judge(type, collector);
            mHandler.postDelayed(() -> {
                log("type " + type + ": events after unregister " + (collector.events.size() - before));
                next();
            }, 400);
        }, 1500);
    }

    private void judge(int type, Collector c) {
        final int count = c.events.size();
        boolean lengths = count > 0;
        boolean increasing = true;
        boolean fresh = count > 0;
        boolean finite = true;
        for (int i = 0; i < count; i++) {
            final float[] v = c.copies.get(i);
            lengths &= v != null && v.length >= c.values;
            if (v != null) {
                for (int k = 0; k < Math.min(v.length, c.values); k++) {
                    finite &= !Float.isNaN(v[k]) && !Float.isInfinite(v[k]);
                }
            }
            if (i > 0) {
                increasing &= c.stamps.get(i) >= c.stamps.get(i - 1);
            }
            // a timestamp is on the elapsedRealtimeNanos base: not after its arrival, and at most a second before it
            final long age = c.arrivals.get(i) - c.stamps.get(i);
            fresh &= age >= -10_000_000L && age < 1_000_000_000L;
        }
        log("type " + type + ": events " + (count >= 5) + ", values " + lengths + ", finite " + finite
                + ", timestamps increasing " + increasing + ", timestamps fresh " + fresh + ", same sensor "
                + c.sameSensor + ", main thread " + !c.wrongThread);
    }
}
