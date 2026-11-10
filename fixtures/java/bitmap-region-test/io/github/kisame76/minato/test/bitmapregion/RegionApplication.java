package io.github.kisame76.minato.test.bitmapregion;

import android.app.Application;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.BitmapRegionDecoder;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.util.Log;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;

/**
 * A tiled image viewer's calls on BitmapRegionDecoder (a large image shown in tiles): an image of four colored
 * quadrants, 640 by 480, as a PNG and a JPEG, opened from bytes, a stream, a path and a file descriptor; regions of the
 * four quadrants at three sample sizes, with the size and whether the middle pixel has the quadrant's color within a
 * tolerance (the JPEG's colors differ a little between encoders); a 16-bit config; a region partly outside, cropped,
 * and one wholly outside, which throws.
 */
public class RegionApplication extends Application {
    private static final String TAG = "bitmap-region-test";
    private static final int WIDTH = 640, HEIGHT = 480;
    private static final int[] COLORS = {Color.RED, Color.GREEN, Color.BLUE, Color.YELLOW};

    @Override
    public void onCreate() {
        super.onCreate();
        new Handler(Looper.getMainLooper()).post(this::run);
    }

    private static byte[] encode(Bitmap.CompressFormat format) {
        final Bitmap image = Bitmap.createBitmap(WIDTH, HEIGHT, Bitmap.Config.ARGB_8888);
        final Canvas canvas = new Canvas(image);
        final Paint paint = new Paint();
        for (int i = 0; i < 4; i++) {
            paint.setColor(COLORS[i]);
            canvas.drawRect((i % 2) * WIDTH / 2f, (i / 2) * HEIGHT / 2f, (i % 2 + 1) * WIDTH / 2f,
                    (i / 2 + 1) * HEIGHT / 2f, paint);
        }
        final ByteArrayOutputStream out = new ByteArrayOutputStream();
        image.compress(format, 100, out);
        return out.toByteArray();
    }

    private static boolean near(int a, int b) {
        return Math.abs(Color.red(a) - Color.red(b)) < 48 && Math.abs(Color.green(a) - Color.green(b)) < 48
                && Math.abs(Color.blue(a) - Color.blue(b)) < 48;
    }

    private void run() {
        try {
            report("png", encode(Bitmap.CompressFormat.PNG));
            report("jpeg", encode(Bitmap.CompressFormat.JPEG));
        } catch (Throwable t) {
            Log.i(TAG, "failed: " + t.getClass().getName());
            Log.i(TAG + "-device", Log.getStackTraceString(t));
        }
        Log.i(TAG, "done");
    }

    private void report(String kind, byte[] bytes) throws IOException {
        final BitmapRegionDecoder decoder = BitmapRegionDecoder.newInstance(bytes, 0, bytes.length);
        Log.i(TAG, kind + " from bytes: " + decoder.getWidth() + "x" + decoder.getHeight());
        for (int sample : new int[] {1, 2, 4}) {
            final BitmapFactory.Options options = new BitmapFactory.Options();
            options.inSampleSize = sample;
            for (int i = 0; i < 4; i++) {
                final int left = (i % 2) * WIDTH / 2, top = (i / 2) * HEIGHT / 2;
                final Rect region = new Rect(left, top, left + WIDTH / 2, top + HEIGHT / 2);
                final Bitmap tile = decoder.decodeRegion(region, options);
                final int middle = tile.getPixel(tile.getWidth() / 2, tile.getHeight() / 2);
                Log.i(TAG, kind + " quadrant " + i + " sample " + sample + ": " + tile.getWidth() + "x"
                        + tile.getHeight() + ", color right " + near(middle, COLORS[i]));
            }
        }
        final BitmapFactory.Options small = new BitmapFactory.Options();
        small.inPreferredConfig = Bitmap.Config.RGB_565;
        final Bitmap tile565 = decoder.decodeRegion(new Rect(0, 0, 100, 100), small);
        Log.i(TAG, kind + " 565 config: " + tile565.getConfig());
        final Bitmap cropped = decoder.decodeRegion(new Rect(600, 440, 700, 520), null);
        Log.i(TAG, kind + " partly outside: " + cropped.getWidth() + "x" + cropped.getHeight());
        try {
            decoder.decodeRegion(new Rect(700, 500, 800, 600), null);
            Log.i(TAG, kind + " wholly outside: no exception");
        } catch (IllegalArgumentException e) {
            Log.i(TAG, kind + " wholly outside: IllegalArgumentException");
        }
        decoder.recycle();
        Log.i(TAG, kind + " recycled: " + decoder.isRecycled());
        final BitmapRegionDecoder stream = BitmapRegionDecoder.newInstance(new ByteArrayInputStream(bytes));
        Log.i(TAG, kind + " from a stream: " + stream.getWidth() + "x" + stream.getHeight());
        stream.recycle();
        final File file = new File(getCacheDir(), "region." + kind);
        try (FileOutputStream out = new FileOutputStream(file)) {
            out.write(bytes);
        }
        final BitmapRegionDecoder path = BitmapRegionDecoder.newInstance(file.getPath());
        Log.i(TAG, kind + " from a path: " + path.getWidth() + "x" + path.getHeight());
        path.recycle();
        try (ParcelFileDescriptor in = ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY)) {
            final BitmapRegionDecoder descriptor = BitmapRegionDecoder.newInstance(in);
            Log.i(TAG, kind + " from a descriptor: " + descriptor.getWidth() + "x" + descriptor.getHeight());
            descriptor.recycle();
        }
    }
}
