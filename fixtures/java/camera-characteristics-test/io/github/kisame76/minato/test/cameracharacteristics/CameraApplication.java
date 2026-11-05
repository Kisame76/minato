package io.github.kisame76.minato.test.cameracharacteristics;

import android.app.Application;
import android.content.Context;
import android.content.pm.PackageManager;
import android.hardware.Camera;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.CaptureResult;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;

import java.lang.reflect.Array;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Set;
import java.util.TreeSet;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * ADR-0073's characteristics dump. Android 15's emulator (one FULL back camera, "1") is the API's reference, not
 * Minato's model: Minato offers "0" back and "1" front at LIMITED with BACKWARD_COMPATIBLE (ADR-0073 decision 1). So
 * a check of Minato's log against Android 15's compares the key set and the values' types, and the keys LIMITED
 * requires, against the emulator's, and the values by the model's rules (ranges, not equality), as the plan's risk
 * section says. The main tag has the answers that do not depend on the device (the features, the keys and their
 * types, the opens' answers); the values go to the device tag. From a thread that Application.attachBaseContext
 * starts:
 * - the camera features PackageManager reports;
 * - CameraManager's cameras and concurrent sets; for each camera every characteristics key with its value's type, the
 *   keys that need the CAMERA permission, and the request and result keys it lists;
 * - with the permission (camera-characteristics-test-granted, which the harness grants), each camera opened and
 *   closed through camera2, else the open's exception;
 * - the legacy Camera's cameras (facing, orientation, shutter sound), and each opened with its parameters' keys
 *   (values to the device tag), else the open's exception.
 * Each wait is bounded; it logs with the tag camera-characteristics-test (the twin with its own name) and ends with
 * "done".
 */
public class CameraApplication extends Application {
    private static String TAG = "camera-characteristics-test";

    private final List<String> lines = new ArrayList<>();

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        if (base.getPackageName().endsWith("granted")) TAG = "camera-characteristics-test-granted";  // the twin
        new Thread(() -> {
            try {
                test(base);
            } catch (Throwable t) {
                add("failed: " + t);
                add("device " + Log.getStackTraceString(t));
            }
            add("done");
            flush();
        }, TAG).start();
    }

    private void add(String line) {
        lines.add(line);
    }

    /** The lines, those that start with "device " to the device tag, a pause after every 10 (liblog drops lines). */
    private void flush() {
        int count = 0;
        for (String line : lines) {
            boolean device = line.startsWith("device ");
            String text = device ? line.substring(7) : line;
            // Long values in parts, as logcat cuts a line at about 4000 bytes.
            for (int at = 0; at < text.length() || at == 0; at += 900) {
                String part = text.substring(at, Math.min(text.length(), at + 900));
                Log.i(device ? TAG + "-device" : TAG, (at > 0 ? "... " : "") + part);
                if (++count % 10 == 0) pause(5);
            }
        }
        lines.clear();
    }

    private static void pause(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    /** A value as text: arrays element by element, everything else by its toString. */
    private static String text(Object value) {
        if (value == null) return "null";
        if (!value.getClass().isArray()) return String.valueOf(value);
        StringBuilder out = new StringBuilder("[");
        for (int i = 0; i < Array.getLength(value); i++) {
            out.append(i == 0 ? "" : ", ").append(text(Array.get(value, i)));
        }
        return out.append(']').toString();
    }

    private static String type(Object value) {
        return value == null ? "null" : value.getClass().getSimpleName();
    }

    private static <T> String names(List<T> keys, java.util.function.Function<T, String> name) {
        List<String> sorted = new ArrayList<>();
        for (T key : keys) sorted.add(name.apply(key));
        Collections.sort(sorted);
        return sorted.size() + " " + sorted;
    }

    private void test(Context context) throws Exception {
        PackageManager packages = context.getPackageManager();
        for (String feature : new String[] {PackageManager.FEATURE_CAMERA, PackageManager.FEATURE_CAMERA_ANY,
                PackageManager.FEATURE_CAMERA_FRONT, PackageManager.FEATURE_CAMERA_AUTOFOCUS,
                PackageManager.FEATURE_CAMERA_FLASH, PackageManager.FEATURE_CAMERA_LEVEL_FULL,
                PackageManager.FEATURE_CAMERA_CAPABILITY_MANUAL_SENSOR,
                PackageManager.FEATURE_CAMERA_CAPABILITY_MANUAL_POST_PROCESSING,
                PackageManager.FEATURE_CAMERA_CAPABILITY_RAW, PackageManager.FEATURE_CAMERA_EXTERNAL,
                PackageManager.FEATURE_CAMERA_CONCURRENT}) {
            add("feature " + feature + ": " + packages.hasSystemFeature(feature));
        }
        add("permission CAMERA granted: "
                + (context.checkSelfPermission(android.Manifest.permission.CAMERA)
                        == PackageManager.PERMISSION_GRANTED));
        flush();
        camera2(context);
        flush();
        legacy();
    }

    private void camera2(Context context) throws Exception {
        CameraManager manager = context.getSystemService(CameraManager.class);
        String[] ids = manager.getCameraIdList();
        add("camera2: " + ids.length + " cameras " + Arrays.toString(ids));
        List<String> sets = new ArrayList<>();
        for (Set<String> set : manager.getConcurrentCameraIds()) sets.add(new TreeSet<>(set).toString());
        Collections.sort(sets);
        add("camera2: concurrent " + sets);
        HandlerThread thread = new HandlerThread("camera");
        thread.start();
        Handler handler = new Handler(thread.getLooper());
        for (String id : ids) {
            CameraCharacteristics characteristics = manager.getCameraCharacteristics(id);
            String prefix = "camera2 " + id;
            add(prefix + ": facing " + characteristics.get(CameraCharacteristics.LENS_FACING) + ", orientation "
                    + characteristics.get(CameraCharacteristics.SENSOR_ORIENTATION) + ", hardware level "
                    + characteristics.get(CameraCharacteristics.INFO_SUPPORTED_HARDWARE_LEVEL));
            add(prefix + ": capabilities "
                    + text(characteristics.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES)));
            List<CameraCharacteristics.Key<?>> keys = new ArrayList<>(characteristics.getKeys());
            keys.sort((a, b) -> a.getName().compareTo(b.getName()));
            for (CameraCharacteristics.Key<?> key : keys) {
                Object value = characteristics.get(key);
                add(prefix + " key " + key.getName() + ": " + type(value));
                add("device " + prefix + " " + key.getName() + " = " + text(value));
            }
            add(prefix + ": keys needing the permission "
                    + names(characteristics.getKeysNeedingPermission(), CameraCharacteristics.Key::getName));
            add(prefix + ": request keys "
                    + names(characteristics.getAvailableCaptureRequestKeys(), CaptureRequest.Key::getName));
            add(prefix + ": result keys "
                    + names(characteristics.getAvailableCaptureResultKeys(), CaptureResult.Key::getName));
            open(manager, id, handler, prefix);
        }
        thread.quitSafely();
    }

    /** camera2's open of a camera and its close, or the open's exception. */
    private void open(CameraManager manager, String id, Handler handler, String prefix) throws Exception {
        CountDownLatch opened = new CountDownLatch(1), closed = new CountDownLatch(1);
        String[] answer = {"none"};
        try {
            manager.openCamera(id, new CameraDevice.StateCallback() {
                @Override
                public void onOpened(CameraDevice camera) {
                    answer[0] = "opened " + camera.getId();
                    opened.countDown();
                    camera.close();
                }

                @Override
                public void onClosed(CameraDevice camera) {
                    closed.countDown();
                }

                @Override
                public void onDisconnected(CameraDevice camera) {
                    answer[0] = "disconnected";
                    opened.countDown();
                }

                @Override
                public void onError(CameraDevice camera, int error) {
                    answer[0] = "error " + error;
                    opened.countDown();
                }
            }, handler);
        } catch (Exception e) {
            // The message names the app's process and uid, the device's.
            add(prefix + ": open " + e.getClass().getName());
            add("device " + prefix + ": " + e.getMessage());
            return;
        }
        boolean came = opened.await(10, TimeUnit.SECONDS);
        flush();
        add(prefix + ": open " + (came ? answer[0] : "no answer in 10 s") + ", closed "
                + (came && answer[0].startsWith("opened") && closed.await(10, TimeUnit.SECONDS)));
    }

    private void legacy() {
        int count = Camera.getNumberOfCameras();
        add("legacy: " + count + " cameras");
        for (int id = 0; id < count; id++) {
            Camera.CameraInfo info = new Camera.CameraInfo();
            Camera.getCameraInfo(id, info);
            add("legacy " + id + ": facing " + info.facing + ", orientation " + info.orientation
                    + ", can disable the shutter sound " + info.canDisableShutterSound);
            flush();
            Camera camera;
            try {
                camera = Camera.open(id);
            } catch (RuntimeException e) {
                add("legacy " + id + ": open " + e.getClass().getName() + " " + e.getMessage());
                continue;
            }
            List<String> entries = new ArrayList<>(Arrays.asList(camera.getParameters().flatten().split(";")));
            Collections.sort(entries);
            List<String> keys = new ArrayList<>();
            for (String entry : entries) {
                int equals = entry.indexOf('=');
                keys.add(equals < 0 ? entry : entry.substring(0, equals));
                add("device legacy " + id + " " + entry);
            }
            add("legacy " + id + ": parameters " + keys.size() + " " + keys);
            camera.release();
        }
    }
}
