package io.github.kisame76.minato.test.cameralegacy;

import android.app.Application;
import android.content.Context;
import android.graphics.ImageFormat;
import android.hardware.Camera;
import android.util.Log;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/**
 * ADR-0073 step e's test of the legacy android.hardware.Camera, with CAMERA granted. Android 15's emulator's camera is
 * not Minato's, so the main tag holds only the answers every Android 15 camera gives (each check as true or false, the
 * exceptions by class and message) and the values go to the device tag. From a thread that
 * Application.attachBaseContext starts:
 * - the cameras: at least one, each facing back or front with an orientation of a multiple of 90, and the answer to an
 *   ID past them;
 * - the first camera opened; its parameters: the keys every camera's have (cameraservice's Parameters::initialize sets
 *   them whatever the camera), and the getters' answers against their own lists (the preview and picture sizes and
 *   formats, the frame rate range, the focus mode, the zoom ratios), and a flatten and unflatten of them;
 * - setParameters with the smallest preview size, which getParameters gives back, and with a size the camera lacks,
 *   which fails and keeps the parameters;
 * - lock, unlock and reconnect; an open of an ID past the cameras; a second open of the first camera, which takes it
 *   from the first Camera (its getParameters then fails); a release, after which getParameters fails, and an open
 *   again.
 * It logs with the tag camera-legacy-test and ends with "done".
 */
public class LegacyApplication extends Application {
    private static final String TAG = "camera-legacy-test";

    /** The keys of Parameters::initialize that do not depend on a camera's flash, focus, scenes or zoom. */
    private static final String[] REQUIRED = {
        "antibanding", "antibanding-values", "auto-exposure-lock-supported", "auto-whitebalance-lock-supported",
        "effect", "effect-values", "exposure-compensation", "exposure-compensation-step", "focal-length",
        "focus-areas", "focus-distances", "focus-mode", "focus-mode-values", "horizontal-view-angle", "jpeg-quality",
        "jpeg-thumbnail-height", "jpeg-thumbnail-quality", "jpeg-thumbnail-size-values", "jpeg-thumbnail-width",
        "max-exposure-compensation", "max-num-detected-faces-hw", "max-num-detected-faces-sw", "max-num-focus-areas",
        "max-num-metering-areas", "metering-areas", "min-exposure-compensation", "picture-format",
        "picture-format-values", "picture-size", "picture-size-values", "preferred-preview-size-for-video",
        "preview-format", "preview-format-values", "preview-fps-range", "preview-fps-range-values",
        "preview-frame-rate", "preview-frame-rate-values", "preview-size", "preview-size-values", "recording-hint",
        "rotation", "smooth-zoom-supported", "vertical-view-angle", "video-frame-format", "video-size",
        "video-size-values", "video-snapshot-supported", "video-stabilization", "video-stabilization-supported",
        "whitebalance", "whitebalance-values", "zoom-supported",
    };

    private final List<String> lines = new ArrayList<>();

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        new Thread(() -> {
            try {
                test();
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
            for (int at = 0; at < text.length() || at == 0; at += 900) {
                Log.i(device ? TAG + "-device" : TAG,
                        (at > 0 ? "... " : "") + text.substring(at, Math.min(text.length(), at + 900)));
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

    private static String thrown(Throwable t) {
        return t.getClass().getName() + " " + t.getMessage();
    }

    private void test() {
        int count = Camera.getNumberOfCameras();
        add("cameras: at least one " + (count > 0));
        add("device cameras: " + count);
        boolean facing = true, orientation = true;
        for (int id = 0; id < count; id++) {
            Camera.CameraInfo info = new Camera.CameraInfo();
            Camera.getCameraInfo(id, info);
            facing &= info.facing == Camera.CameraInfo.CAMERA_FACING_BACK
                    || info.facing == Camera.CameraInfo.CAMERA_FACING_FRONT;
            orientation &= info.orientation % 90 == 0 && info.orientation >= 0 && info.orientation < 360;
            add("device camera " + id + ": facing " + info.facing + ", orientation " + info.orientation);
        }
        add("every camera: facing back or front " + facing + ", orientation a multiple of 90 " + orientation);
        try {
            Camera.getCameraInfo(count, new Camera.CameraInfo());
            add("camera past the last: no exception");
        } catch (RuntimeException e) {
            add("camera past the last: " + thrown(e));
        }
        if (count == 0) return;

        Camera camera = Camera.open(0);
        add("open 0: " + (camera != null));
        parameters(camera);
        try {
            Camera.open(count).release();
            add("open past the last: opened");
        } catch (RuntimeException e) {
            add("open past the last: " + thrown(e));
        }
        // A second open of the camera takes it from the first, whose calls then fail.
        Camera again = null;
        try {
            again = Camera.open(0);
            add("open 0 again: opened");
        } catch (RuntimeException e) {
            add("open 0 again: " + thrown(e));
        }
        try {
            camera.getParameters();
            add("the first after the second open: parameters");
        } catch (RuntimeException e) {
            add("the first after the second open: " + thrown(e));
        }
        if (again != null) again.release();

        camera.release();
        try {
            camera.getParameters();
            add("parameters after release: no exception");
        } catch (RuntimeException e) {
            add("parameters after release: " + thrown(e));
        }
        Camera reopened = Camera.open(0);
        add("open 0 after release: " + (reopened != null));
        reopened.release();
    }

    private void parameters(Camera camera) {
        Camera.Parameters parameters = camera.getParameters();
        String flattened = parameters.flatten();
        List<String> keys = new ArrayList<>();
        for (String entry : flattened.split(";")) {
            int equals = entry.indexOf('=');
            keys.add(equals < 0 ? entry : entry.substring(0, equals));
        }
        List<String> missing = new ArrayList<>();
        for (String key : REQUIRED) {
            if (!keys.contains(key)) missing.add(key);
        }
        add("parameters: required keys missing " + missing);
        List<String> sorted = new ArrayList<>(keys);
        Collections.sort(sorted);
        add("device parameters: " + sorted.size() + " keys " + sorted);
        add("device parameters: " + flattened);

        Camera.Size preview = parameters.getPreviewSize(), picture = parameters.getPictureSize();
        add("preview size listed " + parameters.getSupportedPreviewSizes().contains(preview)
                + ", picture size listed " + parameters.getSupportedPictureSizes().contains(picture));
        add("preview format NV21 " + (parameters.getPreviewFormat() == ImageFormat.NV21)
                + ", preview formats with NV21 and YV12 "
                + parameters.getSupportedPreviewFormats().containsAll(Arrays.asList(ImageFormat.NV21, ImageFormat.YV12))
                + ", picture formats " + parameters.getSupportedPictureFormats()
                + ", picture format JPEG " + (parameters.getPictureFormat() == ImageFormat.JPEG));
        int[] range = new int[2];
        parameters.getPreviewFpsRange(range);
        boolean listed = false;
        for (int[] each : parameters.getSupportedPreviewFpsRange()) listed |= Arrays.equals(each, range);
        add("preview fps range listed " + listed + ", focus mode listed "
                + parameters.getSupportedFocusModes().contains(parameters.getFocusMode())
                + ", jpeg quality " + parameters.getJpegQuality() + ", thumbnail quality "
                + parameters.getJpegThumbnailQuality() + ", rotation " + parameters.get("rotation"));
        if (parameters.isZoomSupported()) {
            List<Integer> ratios = parameters.getZoomRatios();
            add("zoom: ratios " + (ratios.size() == parameters.getMaxZoom() + 1) + ", the first 100 "
                    + (ratios.get(0) == 100) + ", zoom " + parameters.getZoom());
        } else {
            add("zoom: not supported");
        }
        Camera.Parameters copy = camera.getParameters();
        copy.unflatten(flattened);
        add("flatten and unflatten: same " + copy.flatten().equals(flattened));

        Camera.Size smallest = null;
        for (Camera.Size size : parameters.getSupportedPreviewSizes()) {
            if (smallest == null || size.width * size.height < smallest.width * smallest.height) smallest = size;
        }
        parameters.setPreviewSize(smallest.width, smallest.height);
        camera.setParameters(parameters);
        add("set the smallest preview size: given back "
                + camera.getParameters().getPreviewSize().equals(smallest));
        Camera.Parameters wrong = camera.getParameters();
        wrong.setPreviewSize(1, 1);
        try {
            camera.setParameters(wrong);
            add("set a preview size of 1x1: no exception");
        } catch (RuntimeException e) {
            add("set a preview size of 1x1: " + thrown(e));
        }
        add("after the failed set: the smallest still "
                + camera.getParameters().getPreviewSize().equals(smallest));

        try {
            camera.lock();
            camera.unlock();
            camera.reconnect();
            add("lock, unlock and reconnect: no exception");
        } catch (Exception e) {
            add("lock, unlock and reconnect: " + thrown(e));
        }
    }
}
