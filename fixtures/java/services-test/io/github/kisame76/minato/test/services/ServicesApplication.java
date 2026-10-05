package io.github.kisame76.minato.test.services;

import android.app.AppOpsManager;
import android.app.Application;
import android.content.Context;
import android.content.pm.PackageManager;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.net.wifi.WifiManager;
import android.os.Environment;
import android.os.Process;
import android.os.storage.StorageManager;
import android.os.storage.StorageVolume;
import android.speech.tts.UtteranceProgressListener;
import android.util.Log;

import java.util.ArrayList;
import java.util.List;

/**
 * ADR-0045's test of the local services that Godot's start asks for (gaps 11, 12 and 14 of the Godot survey), in
 * Application.onCreate, before its launcher activity starts:
 * <ul>
 *   <li>a subclass of UtteranceProgressListener, whose default onError(id, code) calls the older onError(id)
 *       and whose other defaults do nothing;
 *   <li>the sensor service on a device without sensors: its class, the list, the default sensors and a listener
 *       without a sensor;
 *   <li>the Wi-Fi feature, and the Wi-Fi service's multicast lock: held and released by its reference count,
 *       under-locked, without a reference count, and refused past 50 locks; a Wi-Fi lock without WAKE_LOCK, refused
 *       at its acquire and at its release;
 *   <li>whether Wi-Fi is on, which the service refuses without ACCESS_WIFI_STATE, as the app does not request it;
 *   <li>the storage service (stop 4): its one volume, the primary emulated one on both, found again from the app's
 *       external files, and whether the app has legacy storage or manages all files;
 *   <li>the app ops service (stop 6): the modes of a runtime permission's op that the app requests and is denied
 *       (CAMERA), of one it does not request (RECORD_AUDIO), of all-files access, picture in picture and no isolated
 *       storage; a note; a check that throws for an errored op; an unknown op.
 * </ul>
 * apk-runner-35-bare has no sensors, as Minato has none, so every line is the same on both; an exception's message,
 * which names the app's uid, is left out.
 */
public class ServicesApplication extends Application {
    private static final String TAG = "services-test";

    private interface Section {
        void run() throws Exception;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        guarded("UtteranceProgressListener", this::speechListener);
        guarded("sensor service", this::sensors);
        guarded("wifi service", this::wifiLocks);
        guarded("storage service", this::storage);
        guarded("app ops service", this::appOps);
        Log.i(TAG, "done");
    }

    private void speechListener() {
        final StringBuilder calls = new StringBuilder();
        UtteranceProgressListener listener = new UtteranceProgressListener() {
            @Override
            public void onStart(String utteranceId) {
                calls.append(" onStart(").append(utteranceId).append(')');
            }

            @Override
            public void onDone(String utteranceId) {
                calls.append(" onDone(").append(utteranceId).append(')');
            }

            @Override
            public void onError(String utteranceId) {
                calls.append(" onError(").append(utteranceId).append(')');
            }
        };
        // TextToSpeech.ERROR_NETWORK, written out so that TextToSpeech, which Minato does not have yet, is not named.
        listener.onError("u", -6);
        listener.onRangeStart("u", 1, 2, 3);
        listener.onStop("u", true);
        listener.onBeginSynthesis("u", 16000, 2, 1);
        listener.onAudioAvailable("u", new byte[0]);
        Log.i(TAG, "UtteranceProgressListener's defaults called:" + calls);
    }

    private void sensors() {
        SensorManager sensors = (SensorManager) getSystemService(Context.SENSOR_SERVICE);
        Log.i(TAG, "sensor service: " + sensors.getClass().getName());
        Log.i(TAG, "sensors: " + sensors.getSensorList(Sensor.TYPE_ALL).size() + ", dynamic discovery "
                + sensors.isDynamicSensorDiscoverySupported());
        Log.i(TAG, "default sensors: accelerometer " + sensors.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
                + ", gyroscope " + sensors.getDefaultSensor(Sensor.TYPE_GYROSCOPE) + ", magnetic field "
                + sensors.getDefaultSensor(Sensor.TYPE_MAGNETIC_FIELD) + ", gravity "
                + sensors.getDefaultSensor(Sensor.TYPE_GRAVITY));
        SensorEventListener listener = new SensorEventListener() {
            @Override
            public void onSensorChanged(SensorEvent event) {
            }

            @Override
            public void onAccuracyChanged(Sensor sensor, int accuracy) {
            }
        };
        Log.i(TAG, "registerListener without a sensor: "
                + sensors.registerListener(listener, null, SensorManager.SENSOR_DELAY_GAME));
    }

    private void wifiLocks() {
        Log.i(TAG, "Wi-Fi feature: " + getPackageManager().hasSystemFeature(PackageManager.FEATURE_WIFI));
        WifiManager wifi = (WifiManager) getSystemService(Context.WIFI_SERVICE);
        Log.i(TAG, "wifi service: " + wifi.getClass().getName());
        try {
            Log.i(TAG, "isWifiEnabled without ACCESS_WIFI_STATE: " + wifi.isWifiEnabled());
        } catch (SecurityException e) {
            Log.i(TAG, "isWifiEnabled without ACCESS_WIFI_STATE: " + e.getClass().getName());
        }
        WifiManager.MulticastLock lock = wifi.createMulticastLock(TAG);
        StringBuilder held = new StringBuilder("multicast lock held: " + lock.isHeld());
        lock.acquire();
        held.append(", after acquire ").append(lock.isHeld());
        lock.acquire();
        lock.release();
        held.append(", after a second acquire and one release ").append(lock.isHeld());
        lock.release();
        held.append(", after the second release ").append(lock.isHeld());
        Log.i(TAG, held.toString());
        guarded("one release more", () -> lock.release());

        WifiManager.MulticastLock plain = wifi.createMulticastLock(TAG + "-plain");
        plain.setReferenceCounted(false);
        plain.acquire();
        plain.acquire();
        plain.release();
        Log.i(TAG, "without a reference count, acquired twice and released once: held " + plain.isHeld());

        List<WifiManager.MulticastLock> locks = new ArrayList<>();
        try {
            for (int i = 0; i < 51; i++) {
                WifiManager.MulticastLock next = wifi.createMulticastLock(TAG + "-" + i);
                next.acquire();
                locks.add(next);
            }
            Log.i(TAG, "51 multicast locks held");
        } catch (RuntimeException e) {
            Log.i(TAG, "multicast lock " + (locks.size() + 1) + ": " + e);
        }
        for (WifiManager.MulticastLock each : locks) {
            each.release();
        }

        WifiManager.WifiLock wifiLock = wifi.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, TAG);
        try {
            wifiLock.acquire();
            Log.i(TAG, "Wi-Fi lock without WAKE_LOCK: held " + wifiLock.isHeld());
        } catch (SecurityException e) {
            Log.i(TAG, "Wi-Fi lock without WAKE_LOCK: " + e.getClass().getName() + ", held " + wifiLock.isHeld());
        }
        try {
            wifiLock.release();
            Log.i(TAG, "Wi-Fi lock release without WAKE_LOCK: held " + wifiLock.isHeld());
        } catch (SecurityException e) {
            Log.i(TAG, "Wi-Fi lock release without WAKE_LOCK: " + e.getClass().getName());
        }
    }

    private void storage() {
        StorageManager storage = (StorageManager) getSystemService(Context.STORAGE_SERVICE);
        Log.i(TAG, "storage service: " + storage.getClass().getName());
        List<StorageVolume> volumes = storage.getStorageVolumes();
        Log.i(TAG, "storage volumes: " + volumes.size());
        StorageVolume volume = volumes.get(0);
        Log.i(TAG, "volume: " + volume.getDescription(this) + ", directory " + volume.getDirectory() + ", primary "
                + volume.isPrimary() + ", removable " + volume.isRemovable() + ", emulated " + volume.isEmulated()
                + ", state " + volume.getState());
        Log.i(TAG, "volume: uuid " + volume.getUuid() + ", storage uuid " + volume.getStorageUuid()
                + ", media store volume " + volume.getMediaStoreVolumeName());
        Log.i(TAG, "the app's external files on it: "
                + volume.equals(storage.getStorageVolume(getExternalFilesDir(null))) + ", the primary volume "
                + volume.equals(storage.getPrimaryStorageVolume()));
        Log.i(TAG, "legacy external storage: " + Environment.isExternalStorageLegacy()
                + ", external storage manager: " + Environment.isExternalStorageManager());
    }

    private void appOps() {
        AppOpsManager appOps = (AppOpsManager) getSystemService(Context.APP_OPS_SERVICE);
        Log.i(TAG, "app ops service: " + appOps.getClass().getName());
        int uid = Process.myUid();
        String name = getPackageName();
        StringBuilder modes = new StringBuilder("app op modes:");
        // Written out where the op's name is not public API.
        for (String op : new String[] {AppOpsManager.OPSTR_CAMERA, AppOpsManager.OPSTR_RECORD_AUDIO,
                "android:manage_external_storage", AppOpsManager.OPSTR_PICTURE_IN_PICTURE,
                "android:no_isolated_storage"}) {
            modes.append(' ').append(op).append(' ').append(appOps.unsafeCheckOpNoThrow(op, uid, name));
        }
        Log.i(TAG, modes.toString());
        Log.i(TAG, "camera noted: " + appOps.noteOpNoThrow(AppOpsManager.OPSTR_CAMERA, uid, name, null, null));
        try {
            Log.i(TAG, "no isolated storage checked: "
                    + appOps.unsafeCheckOp("android:no_isolated_storage", uid, name));
        } catch (SecurityException e) {
            Log.i(TAG, "no isolated storage checked: " + e.getClass().getName());
        }
        guarded("an unknown op", () -> Log.i(TAG, "an unknown op: "
                + appOps.unsafeCheckOpNoThrow("android:no_such_op", uid, name)));
    }

    /** Runs a section; what it throws is logged with its name. */
    private static void guarded(String name, Section section) {
        try {
            section.run();
        } catch (Throwable e) {
            Log.i(TAG, name + ": " + e);
        }
    }
}
