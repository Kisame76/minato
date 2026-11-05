package io.github.kisame76.minato.test.camerapicture;

import android.app.Application;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.SurfaceTexture;
import android.hardware.Camera;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * ADR-0073 step e's test of the legacy android.hardware.Camera's focus and pictures, with CAMERA granted. The main
 * tag holds only the answers every Android 15 camera gives; values go to the device tag. From a thread that
 * Application.attachBaseContext starts, on camera 0 previewing into a detached SurfaceTexture, its callbacks on a
 * HandlerThread's Looper:
 * - autoFocus: its callback within 5 s, and cancelAutoFocus;
 * - takePicture at the smallest picture size with a shutter, a raw and a JPEG callback: their order, the raw one's
 *   data, the JPEG's decode at that size, and the preview stopped after it; startPreview again;
 * - takePicture without a preview, which fails.
 * Each wait is bounded; it logs with the tag camera-picture-test and ends with "done".
 */
public class PictureApplication extends Application {
    private static final String TAG = "camera-picture-test";

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
            for (String line : lines) {
                boolean device = line.startsWith("device ");
                Log.i(device ? TAG + "-device" : TAG, device ? line.substring(7) : line);
            }
        }, TAG).start();
    }

    private void add(String line) {
        lines.add(line);
    }

    private void test() throws Exception {
        HandlerThread callbacks = new HandlerThread("camera");
        callbacks.start();
        Camera[] opened = new Camera[1];
        CountDownLatch ready = new CountDownLatch(1);
        new Handler(callbacks.getLooper()).post(() -> {
            opened[0] = Camera.open(0);
            ready.countDown();
        });
        ready.await(10, TimeUnit.SECONDS);
        Camera camera = opened[0];
        Camera.Parameters parameters = camera.getParameters();
        Camera.Size smallest = null;
        for (Camera.Size size : parameters.getSupportedPictureSizes()) {
            if (smallest == null || size.width * size.height < smallest.width * smallest.height) smallest = size;
        }
        parameters.setPictureSize(smallest.width, smallest.height);
        camera.setParameters(parameters);
        add("device picture size " + smallest.width + "x" + smallest.height + ", focus mode "
                + parameters.getFocusMode());

        try {
            camera.takePicture(null, null, (data, c) -> { });
            add("a picture without a preview: no exception");
        } catch (RuntimeException e) {
            add("a picture without a preview: " + e.getClass().getName() + " " + e.getMessage());
        }

        SurfaceTexture texture = new SurfaceTexture(false);
        camera.setPreviewTexture(texture);
        camera.startPreview();
        Thread.sleep(500);

        CountDownLatch focused = new CountDownLatch(1);
        boolean[] success = new boolean[1];
        camera.autoFocus((ok, c) -> {
            success[0] = ok;
            focused.countDown();
        });
        add("autoFocus: called back within 5 s " + focused.await(5, TimeUnit.SECONDS));
        add("device autoFocus success " + success[0]);
        camera.cancelAutoFocus();
        add("cancelAutoFocus: no exception");

        List<String> order = new ArrayList<>();
        byte[][] jpeg = new byte[1][];
        boolean[] rawNull = new boolean[1];
        CountDownLatch taken = new CountDownLatch(1);
        camera.takePicture(() -> order.add("shutter"), (data, c) -> {
            order.add("raw");
            rawNull[0] = data == null;
        }, (data, c) -> {
            order.add("jpeg");
            jpeg[0] = data;
            taken.countDown();
        });
        add("takePicture: the JPEG within 10 s " + taken.await(10, TimeUnit.SECONDS) + ", callbacks " + order
                + ", raw data null " + rawNull[0]);
        Bitmap bitmap = jpeg[0] == null ? null : BitmapFactory.decodeByteArray(jpeg[0], 0, jpeg[0].length);
        add("the JPEG: decodes at the picture size " + (bitmap != null && bitmap.getWidth() == smallest.width
                && bitmap.getHeight() == smallest.height));
        add("device the JPEG's bytes " + (jpeg[0] == null ? -1 : jpeg[0].length));
        add("preview enabled after the picture " + camera.previewEnabled());
        camera.startPreview();
        add("preview enabled after a start again " + camera.previewEnabled());
        camera.stopPreview();
        camera.release();
        texture.release();
        callbacks.quitSafely();
    }
}
