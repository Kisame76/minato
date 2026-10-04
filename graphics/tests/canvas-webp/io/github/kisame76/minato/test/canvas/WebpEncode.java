package io.github.kisame76.minato.test.canvas;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import java.io.FileOutputStream;
import java.io.IOException;

/**
 * Makes canvas-test's WebP files with Android 15's own encoder (graphics/tests/canvas_images.py --webp), run through
 * app_process on the emulator, since macOS's sips cannot write WebP.
 * Usage: WebpEncode <picture.png> <lossy.webp> <lossless.webp>
 */
public final class WebpEncode {
    public static void main(String[] args) throws IOException {
        Bitmap picture = BitmapFactory.decodeFile(args[0]);
        try (FileOutputStream out = new FileOutputStream(args[1])) {
            picture.compress(Bitmap.CompressFormat.WEBP_LOSSY, 80, out);
        }
        try (FileOutputStream out = new FileOutputStream(args[2])) {
            picture.compress(Bitmap.CompressFormat.WEBP_LOSSLESS, 100, out);
        }
    }
}
