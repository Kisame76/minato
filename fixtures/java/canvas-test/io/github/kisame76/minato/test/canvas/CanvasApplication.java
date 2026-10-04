package io.github.kisame76.minato.test.canvas;

import android.app.Application;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.DashPathEffect;
import android.graphics.LinearGradient;
import android.graphics.Matrix;
import android.graphics.NinePatch;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RadialGradient;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.SweepGradient;
import android.graphics.Typeface;
import android.util.Log;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.util.Locale;

/**
 * ADR-0043 test 2's canvas test: draws shapes, dashed paths, gradients, clips and transforms, a nine-patch, Roboto text
 * and decoded PNG, JPEG and WebP images into a 480 x 480 Bitmap on the CPU, in nine cells of 160 pixels, from the
 * Application class's static initializer as graphics-start-test does. It logs each step under its tag, then "done".
 * The images come from the APK through the class loader, which needs no Context. The pixels go raw, as
 * copyPixelsToBuffer gives them (RGBA, premultiplied), to java.io.tmpdir/canvas-test.rgba, the app's cache folder on
 * Android and in Minato. tools/android-reference.sh takes the log and the pixels from Android 15, and
 * tools/canvas-check.sh compares them (graphics/tests/canvas_compare.py).
 */
public class CanvasApplication extends Application {
    private static final String TAG = "canvas-test";
    private static final int SIZE = 480;
    private static final int CELL = 160;

    static {
        try {
            draw();
        } catch (Throwable e) {
            Log.e(TAG, "FAILED " + e, e);
        }
        Log.i(TAG, "done");
    }

    static void draw() throws Exception {
        Bitmap bitmap = Bitmap.createBitmap(SIZE, SIZE, Bitmap.Config.ARGB_8888);
        // No density: nothing is scaled for a device, on either side.
        bitmap.setDensity(Bitmap.DENSITY_NONE);
        Canvas canvas = new Canvas(bitmap);
        canvas.drawColor(Color.WHITE);
        Log.i(TAG, "bitmap " + bitmap.getWidth() + "x" + bitmap.getHeight() + " " + bitmap.getConfig()
                + ", canvas density " + canvas.getDensity());

        enter(canvas, 0, 0);
        shapes(canvas);
        canvas.restore();
        enter(canvas, 1, 0);
        paths(canvas);
        canvas.restore();
        enter(canvas, 2, 0);
        gradients(canvas);
        canvas.restore();
        enter(canvas, 0, 1);
        clips(canvas);
        canvas.restore();
        enter(canvas, 1, 1);
        ninePatch(canvas, decode("res/drawable-nodpi-v4/frame.9.png"));
        canvas.restore();
        enter(canvas, 2, 1);
        text(canvas);
        canvas.restore();
        enter(canvas, 0, 2);
        image(canvas, "png", decode("assets/picture.png"), true);
        canvas.restore();
        enter(canvas, 1, 2);
        image(canvas, "jpeg", decode("assets/picture.jpg"), false);
        canvas.restore();
        enter(canvas, 2, 2);
        webp(canvas, decode("assets/picture-lossy.webp"), decode("assets/picture-lossless.webp"));
        canvas.restore();

        ByteBuffer pixels = ByteBuffer.allocate(bitmap.getByteCount());
        bitmap.copyPixelsToBuffer(pixels);
        File file = new File(System.getProperty("java.io.tmpdir"), "canvas-test.rgba");
        try (FileOutputStream out = new FileOutputStream(file)) {
            out.write(pixels.array());
        }
        Log.i(TAG, "pixels " + pixels.array().length + " bytes in canvas-test.rgba");
    }

    /** A cell of the grid, clipped, with its origin at its top left; the caller restores. */
    static void enter(Canvas canvas, int column, int row) {
        canvas.save();
        canvas.translate(column * CELL, row * CELL);
        canvas.clipRect(0, 0, CELL, CELL);
    }

    static void shapes(Canvas canvas) {
        Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
        fill.setColor(0xffe53935);
        canvas.drawRect(12, 12, 72, 60, fill);
        fill.setColor(0xff43a047);
        canvas.drawCircle(112, 40, 28, fill);
        fill.setColor(0xff1e88e5);
        canvas.drawOval(new RectF(12, 76, 92, 116), fill);
        // Translucent, over the oval.
        fill.setColor(0x80fdd835);
        canvas.drawRoundRect(new RectF(60, 88, 148, 148), 16, 16, fill);
        Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeWidth(5);
        stroke.setColor(0xff6d4c41);
        canvas.drawLine(10, 150, 150, 124, stroke);
        stroke.setStrokeCap(Paint.Cap.ROUND);
        canvas.drawArc(new RectF(96, 70, 152, 118), 30, 240, false, stroke);
    }

    static void paths(Canvas canvas) {
        Path wave = new Path();
        wave.moveTo(10, 40);
        wave.cubicTo(50, 0, 90, 80, 150, 30);
        Paint dashed = new Paint(Paint.ANTI_ALIAS_FLAG);
        dashed.setStyle(Paint.Style.STROKE);
        dashed.setStrokeWidth(4);
        dashed.setColor(0xff8e24aa);
        dashed.setPathEffect(new DashPathEffect(new float[] {12, 6}, 0));
        canvas.drawPath(wave, dashed);
        // A five-pointed star, filled even-odd, from StrictMath so that both sides compute the same points.
        Path star = new Path();
        for (int point = 0; point < 5; point++) {
            double angle = StrictMath.PI / 2 + point * 4 * StrictMath.PI / 5;
            float x = (float) (80 + 50 * StrictMath.cos(angle));
            float y = (float) (110 - 50 * StrictMath.sin(angle));
            if (point == 0) {
                star.moveTo(x, y);
            } else {
                star.lineTo(x, y);
            }
        }
        star.close();
        star.setFillType(Path.FillType.EVEN_ODD);
        Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
        fill.setColor(0xff00897b);
        canvas.drawPath(star, fill);
        dashed.setColor(Color.BLACK);
        dashed.setStrokeWidth(2);
        dashed.setPathEffect(new DashPathEffect(new float[] {4, 4}, 2));
        canvas.drawPath(star, dashed);
    }

    static void gradients(Canvas canvas) {
        Paint paint = new Paint();
        paint.setShader(new LinearGradient(0, 0, 150, 0, new int[] {0xffff0000, 0xff00ff00, 0xff0000ff}, null,
                Shader.TileMode.CLAMP));
        canvas.drawRect(5, 5, 155, 45, paint);
        paint.setShader(new RadialGradient(45, 100, 40, 0xffffffff, 0xff000080, Shader.TileMode.MIRROR));
        canvas.drawRect(5, 55, 80, 155, paint);
        paint.setShader(new SweepGradient(118, 105, new int[] {0xffff9800, 0xff9c27b0, 0xff03a9f4, 0xffff9800},
                null));
        paint.setAntiAlias(true);
        canvas.drawCircle(118, 105, 36, paint);
    }

    static void clips(Canvas canvas) {
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        canvas.save();
        canvas.translate(80, 52);
        canvas.rotate(30);
        canvas.clipRect(-60, -30, 60, 30);
        for (int stripe = -8; stripe < 8; stripe++) {
            paint.setColor((stripe & 1) == 0 ? 0xff3949ab : 0xfffb8c00);
            canvas.drawRect(stripe * 10, -60, stripe * 10 + 10, 60, paint);
        }
        canvas.restore();
        canvas.save();
        Path circle = new Path();
        circle.addCircle(80, 118, 34, Path.Direction.CW);
        canvas.clipPath(circle);
        Matrix matrix = new Matrix();
        matrix.setSkew(0.3f, 0, 80, 118);
        matrix.postScale(1.2f, 0.9f, 80, 118);
        canvas.concat(matrix);
        paint.setColor(0xff2e7d32);
        canvas.drawRect(40, 80, 120, 156, paint);
        paint.setColor(0xfff9a825);
        canvas.drawRect(60, 100, 100, 136, paint);
        canvas.restore();
    }

    static void ninePatch(Canvas canvas, Bitmap frame) {
        byte[] chunk = frame.getNinePatchChunk();
        Log.i(TAG, "nine-patch " + frame.getWidth() + "x" + frame.getHeight() + ", chunk "
                + NinePatch.isNinePatchChunk(chunk));
        NinePatch patch = new NinePatch(frame, chunk, "frame");
        patch.draw(canvas, new RectF(10, 10, 150, 70));
        patch.draw(canvas, new RectF(30, 80, 130, 150));
    }

    static void text(Canvas canvas) {
        Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        paint.setTypeface(Typeface.DEFAULT);
        paint.setColor(Color.BLACK);
        paint.setTextSize(30);
        canvas.drawText("Minato", 10, 44, paint);
        Paint.FontMetrics metrics = paint.getFontMetrics();
        Log.i(TAG, String.format(Locale.ROOT, "text Minato at 30 px: width %.2f, ascent %.2f, descent %.2f",
                paint.measureText("Minato"), metrics.ascent, metrics.descent));
        paint.setTextSize(18);
        paint.setColor(0xff1565c0);
        canvas.drawText("Canvas 1234567890", 10, 84, paint);
        paint.setTextSize(14);
        paint.setColor(0xff424242);
        paint.setTextSkewX(-0.25f);
        canvas.drawText("The quick brown fox", 10, 120, paint);
    }

    static void image(Canvas canvas, String format, Bitmap picture, boolean lossless) {
        describe(format, picture, lossless);
        canvas.drawBitmap(picture, 8, 8, null);
        canvas.drawBitmap(picture, null, new RectF(76, 76, 156, 156), new Paint(Paint.FILTER_BITMAP_FLAG));
    }

    static void webp(Canvas canvas, Bitmap lossy, Bitmap lossless) {
        describe("webp lossy", lossy, false);
        describe("webp lossless", lossless, true);
        canvas.drawBitmap(lossy, 8, 8, null);
        canvas.drawBitmap(lossless, 88, 88, null);
    }

    /** Its size, config and alpha, and for a lossless format one pixel, which a lossy decoder may round. */
    static void describe(String format, Bitmap picture, boolean lossless) {
        Log.i(TAG, format + " " + picture.getWidth() + "x" + picture.getHeight() + " " + picture.getConfig()
                + ", alpha " + picture.hasAlpha() + (lossless ? ", pixel 30,30 " + color(picture.getPixel(30, 30)) : ""));
    }

    static Bitmap decode(String path) throws IOException {
        try (InputStream in = CanvasApplication.class.getClassLoader().getResourceAsStream(path)) {
            if (in == null) {
                throw new FileNotFoundException(path + " is not in the APK");
            }
            Bitmap bitmap = BitmapFactory.decodeStream(in);
            if (bitmap == null) {
                throw new IOException("BitmapFactory cannot decode " + path);
            }
            bitmap.setDensity(Bitmap.DENSITY_NONE);
            return bitmap;
        }
    }

    static String color(int color) {
        return String.format(Locale.ROOT, "0x%08x", color);
    }
}
