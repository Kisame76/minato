package io.github.kisame76.minato.test.graphicsstart;

import android.app.Application;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.graphics.fonts.Font;
import android.graphics.fonts.SystemFonts;
import java.nio.ByteBuffer;
import java.util.TreeSet;

/**
 * ADR-0043 test 2's first test: hwui's graphics natives, which app_process registers at every start, draw into
 * Bitmaps on the CPU, and the fonts of the system's lists serve text, from the Application class's static initializer
 * as in app-start-test. Each line goes to System.out with the prefix "graphics-start-test: " and says what follows
 * from the sources and our fonts, without a reference device: the pixels the calls set, the default typeface's style,
 * the fonts the native font API lists, and what text does to pixels, widths and glyphs.
 */
public class GraphicsStartApplication extends Application {
    static {
        try {
            report();
        } catch (Throwable e) {
            // A native that is not registered is an UnsatisfiedLinkError: it fails the test with its name and the
            // stack on stderr.
            print("FAILED " + e);
            e.printStackTrace();
        }
    }

    static void print(String line) {
        System.out.println("graphics-start-test: " + line);
    }

    static void report() {
        Bitmap small = Bitmap.createBitmap(8, 8, Bitmap.Config.ARGB_8888);
        print("bitmap " + small.getWidth() + "x" + small.getHeight() + " " + small.getConfig() + " "
                + small.getByteCount() + " bytes, " + small.getRowBytes() + " per row");
        small.setPixel(1, 2, 0xff336699);
        print("setPixel getPixel " + hex(small.getPixel(1, 2)));
        // The bytes in memory, as copyPixelsToBuffer gives them: ARGB_8888's order on Android.
        ByteBuffer bytes = ByteBuffer.allocate(small.getByteCount());
        small.copyPixelsToBuffer(bytes);
        int at = (2 * small.getWidth() + 1) * 4;
        print(String.format("copyPixelsToBuffer bytes %02x %02x %02x %02x", bytes.get(at), bytes.get(at + 1),
                bytes.get(at + 2), bytes.get(at + 3)));
        // Java's int colors go in through createBitmap(int[]) and out through getPixel unchanged, whatever the order.
        Bitmap colors = Bitmap.createBitmap(new int[] {0xff336699, 0xff00ff80}, 2, 1, Bitmap.Config.ARGB_8888);
        ByteBuffer pair = ByteBuffer.allocate(colors.getByteCount());
        colors.copyPixelsToBuffer(pair);
        print(String.format("createBitmap(int[]) getPixel %s %s, bytes %02x %02x %02x %02x %02x %02x %02x %02x",
                hex(colors.getPixel(0, 0)), hex(colors.getPixel(1, 0)), pair.get(0), pair.get(1), pair.get(2),
                pair.get(3), pair.get(4), pair.get(5), pair.get(6), pair.get(7)));
        new Canvas(small).drawColor(0xff00ff00);
        print("drawColor getPixel " + hex(small.getPixel(1, 2)) + " " + hex(small.getPixel(7, 7)));

        print("default weight " + Typeface.DEFAULT.getWeight() + ", italic " + Typeface.DEFAULT.isItalic());
        TreeSet<String> files = new TreeSet<>();
        for (Font font : SystemFonts.getAvailableFonts()) {
            files.add(font.getFile().getName());
        }
        print("available fonts " + files.size() + (files.isEmpty() ? "" : ": " + String.join(" ", files)));

        Bitmap text = Bitmap.createBitmap(320, 96, Bitmap.Config.ARGB_8888);
        Canvas canvas = new Canvas(text);
        canvas.drawColor(Color.WHITE);
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setColor(Color.BLACK);
        paint.setTextSize(64);
        canvas.drawText("Minato", 8, 72, paint);
        int[] pixels = new int[text.getWidth() * text.getHeight()];
        text.getPixels(pixels, 0, text.getWidth(), 0, 0, text.getWidth(), text.getHeight());
        int changed = 0;
        for (int pixel : pixels) {
            if (pixel != Color.WHITE) {
                changed++;
            }
        }
        print("text Minato changes pixels " + (changed > 0) + ", width above 0 " + (paint.measureText("Minato") > 0));
        print("sans-serif iiii narrower than MMMM " + (paint.measureText("iiii") < paint.measureText("MMMM")));
        paint.setTypeface(Typeface.MONOSPACE);
        print("monospace iiii as wide as MMMM " + (paint.measureText("iiii") == paint.measureText("MMMM")));
        paint.setTypeface(Typeface.DEFAULT);
        print("glyph U+65E5 " + paint.hasGlyph("\u65e5") + ", U+1F600 " + paint.hasGlyph("\ud83d\ude00"));
    }

    static String hex(int color) {
        return String.format("0x%08x", color);
    }
}
