package io.github.kisame76.minato.test.hardwarebitmap;

import android.app.Application;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.ImageDecoder;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;

/**
 * The test of Bitmap.Config.HARDWARE bitmaps: an 8x8 image of four colored quarters made as a HARDWARE bitmap by
 * Bitmap.copy, BitmapFactory (inPreferredConfig) and ImageDecoder (ALLOCATOR_HARDWARE and the default allocator),
 * each read back through a software copy (copy(ARGB_8888)), which Android allows for a HARDWARE bitmap: its quarters
 * must be the image's. The lines under the tag hardware-bitmap-test hold on every Android device; the configs the
 * bitmaps report go to hardware-bitmap-test-device (Minato has no hardware bitmaps of its own, so its stay
 * ARGB_8888). Ends with "done".
 */
public class HardwareBitmapApplication extends Application {
    static final String TAG = "hardware-bitmap-test";
    // The quarters' colors: top left, top right, bottom left, bottom right.
    static final int[] QUARTERS = {0xFFFF8000, 0xFF0080FF, 0xFF20C040, 0xFFC020A0};

    private static Bitmap image() {
        Bitmap image = Bitmap.createBitmap(8, 8, Bitmap.Config.ARGB_8888);
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) image.setPixel(x, y, QUARTERS[(y / 4) * 2 + x / 4]);
        }
        return image;
    }

    /** Whether `bitmap`, read back through a software copy, holds the image's quarters. */
    private static boolean right(Bitmap bitmap) {
        Bitmap copy = bitmap.copy(Bitmap.Config.ARGB_8888, false);
        if (copy == null) return false;
        for (int y = 0; y < copy.getHeight(); y++) {
            for (int x = 0; x < copy.getWidth(); x++) {
                if (copy.getPixel(x, y) != QUARTERS[(y / 4) * 2 + x / 4]) return false;
            }
        }
        return true;
    }

    private static void report(String source, Bitmap bitmap) {
        if (bitmap == null) {
            Log.i(TAG, source + ": made false");
            return;
        }
        Log.i(TAG + "-device", source + ": config " + bitmap.getConfig());
        Log.i(TAG, source + ": made true, " + bitmap.getWidth() + "x" + bitmap.getHeight() + ", pixels right "
                + right(bitmap));
    }

    @Override
    public void onCreate() {
        super.onCreate();
        Bitmap image = image();
        ByteArrayOutputStream png = new ByteArrayOutputStream();
        image.compress(Bitmap.CompressFormat.PNG, 100, png);
        byte[] bytes = png.toByteArray();

        report("Bitmap.copy(HARDWARE)", image.copy(Bitmap.Config.HARDWARE, false));
        BitmapFactory.Options options = new BitmapFactory.Options();
        options.inPreferredConfig = Bitmap.Config.HARDWARE;
        report("BitmapFactory with HARDWARE", BitmapFactory.decodeByteArray(bytes, 0, bytes.length, options));
        try {
            report("ImageDecoder with ALLOCATOR_HARDWARE",
                    ImageDecoder.decodeBitmap(ImageDecoder.createSource(ByteBuffer.wrap(bytes)),
                            (decoder, info, source) -> decoder.setAllocator(ImageDecoder.ALLOCATOR_HARDWARE)));
            report("ImageDecoder with its default allocator",
                    ImageDecoder.decodeBitmap(ImageDecoder.createSource(ByteBuffer.wrap(bytes))));
        } catch (Exception e) {
            Log.i(TAG, "ImageDecoder: " + e.getClass().getName());
        }
        Log.i(TAG, "done");
    }
}
