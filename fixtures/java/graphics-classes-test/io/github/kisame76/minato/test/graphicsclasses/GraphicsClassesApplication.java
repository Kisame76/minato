package io.github.kisame76.minato.test.graphicsclasses;

import android.app.Application;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.BitmapRegionDecoder;
import android.graphics.BlendMode;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Gainmap;
import android.graphics.ImageDecoder;
import android.graphics.Mesh;
import android.graphics.MeshSpecification;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.drawable.AnimatedImageDrawable;
import android.graphics.drawable.Drawable;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;

/**
 * The test of the graphics classes whose natives work on a CPU canvas: BitmapRegionDecoder (regions of a large image,
 * as tiled image viewers decode them), Gainmap (the HDR part of a photo, which a phone's camera writes and which
 * BitmapFactory hands over), AnimatedImageDrawable (ImageDecoder's drawable of an animated GIF or WebP, as image
 * loaders show one) and Mesh with MeshSpecification (a custom mesh drawn on a Bitmap's canvas). Every part catches
 * what it throws and logs the exception's class, so a missing native shows as a line and the next part still runs.
 * The lines under the tag graphics-classes-test hold on every Android device; an exception's message goes to
 * graphics-classes-test-device, which is only shown. Ends with "done".
 */
public class GraphicsClassesApplication extends Application {
    static final String TAG = "graphics-classes-test";
    // An 8x8 image's quarters: top left, top right, bottom left, bottom right.
    static final int[] QUARTERS = {0xFFFF8000, 0xFF0080FF, 0xFF20C040, 0xFFC020A0};

    private static Bitmap image() {
        Bitmap image = Bitmap.createBitmap(8, 8, Bitmap.Config.ARGB_8888);
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) image.setPixel(x, y, QUARTERS[(y / 4) * 2 + x / 4]);
        }
        return image;
    }

    /** Whether every pixel of `bitmap` is the quarter's color at (x0 + x * scale, y0 + y * scale) of the image. */
    private static boolean right(Bitmap bitmap, int x0, int y0, int scale) {
        for (int y = 0; y < bitmap.getHeight(); y++) {
            for (int x = 0; x < bitmap.getWidth(); x++) {
                int ix = x0 + x * scale, iy = y0 + y * scale;
                if (bitmap.getPixel(x, y) != QUARTERS[(iy / 4) * 2 + ix / 4]) return false;
            }
        }
        return true;
    }

    private static void regionDecoder(byte[] png) throws Exception {
        BitmapRegionDecoder decoder = BitmapRegionDecoder.newInstance(png, 0, png.length);
        Log.i(TAG, "BitmapRegionDecoder: " + decoder.getWidth() + "x" + decoder.getHeight());
        Bitmap topRight = decoder.decodeRegion(new Rect(4, 0, 8, 4), null);
        Log.i(TAG, "BitmapRegionDecoder region (4,0)-(8,4): " + topRight.getWidth() + "x" + topRight.getHeight()
                + ", pixels right " + right(topRight, 4, 0, 1));
        BitmapFactory.Options half = new BitmapFactory.Options();
        half.inSampleSize = 2;
        Bitmap bottom = decoder.decodeRegion(new Rect(0, 4, 8, 8), half);
        Log.i(TAG, "BitmapRegionDecoder region (0,4)-(8,8) at inSampleSize 2: " + bottom.getWidth() + "x"
                + bottom.getHeight() + ", pixels right " + right(bottom, 0, 4, 2));
        decoder.recycle();
        Log.i(TAG, "BitmapRegionDecoder recycled " + decoder.isRecycled());
    }

    private static void gainmap() {
        Bitmap contents = Bitmap.createBitmap(4, 4, Bitmap.Config.ARGB_8888);
        contents.eraseColor(0xFF808080);
        Gainmap gainmap = new Gainmap(contents);
        gainmap.setRatioMax(4f, 4f, 4f);
        gainmap.setDisplayRatioForFullHdr(4f);
        Bitmap base = image();
        base.setGainmap(gainmap);
        Gainmap read = base.getGainmap();
        Log.i(TAG, "Gainmap: has gainmap " + base.hasGainmap() + ", ratio max " + read.getRatioMax()[0]
                + ", display ratio " + read.getDisplayRatioForFullHdr() + ", contents "
                + read.getGainmapContents().getWidth() + "x" + read.getGainmapContents().getHeight());
        Bitmap copy = base.copy(Bitmap.Config.ARGB_8888, false);
        Log.i(TAG, "Gainmap: a copy has gainmap " + copy.hasGainmap());
    }

    /** A 2x2 GIF of two frames, red then blue, 100 ms each, looping: each pixel a literal code after a clear code. */
    static byte[] gif() {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        int[] head = {'G', 'I', 'F', '8', '9', 'a', 2, 0, 2, 0, 0x81, 0, 0,
                0xFF, 0, 0, 0, 0, 0xFF, 0, 0, 0, 0xFF, 0xFF, 0xFF,
                0x21, 0xFF, 11, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0', 3, 1, 0, 0, 0};
        for (int b : head) out.write(b);
        for (int color = 0; color < 2; color++) {
            int[] frame = {0x21, 0xF9, 4, 0x04, 10, 0, 0, 0, 0x2C, 0, 0, 0, 0, 2, 0, 2, 0, 0, 2};
            for (int b : frame) out.write(b);
            int acc = 0, bits = 0;
            ByteArrayOutputStream data = new ByteArrayOutputStream();
            int[] codes = {4, color, 4, color, 4, color, 4, color, 5};
            for (int code : codes) {
                acc |= code << bits;
                bits += 3;
                while (bits >= 8) {
                    data.write(acc & 0xFF);
                    acc >>>= 8;
                    bits -= 8;
                }
            }
            if (bits > 0) data.write(acc & 0xFF);
            out.write(data.size());
            byte[] bytes = data.toByteArray();
            out.write(bytes, 0, bytes.length);
            out.write(0);
        }
        out.write(0x3B);
        return out.toByteArray();
    }

    private static void animatedImage() throws Exception {
        Drawable drawable = ImageDecoder.decodeDrawable(ImageDecoder.createSource(ByteBuffer.wrap(gif())));
        Log.i(TAG, "AnimatedImageDrawable: drawable " + drawable.getClass().getSimpleName() + ", "
                + drawable.getIntrinsicWidth() + "x" + drawable.getIntrinsicHeight());
        AnimatedImageDrawable animated = (AnimatedImageDrawable) drawable;
        Bitmap target = Bitmap.createBitmap(2, 2, Bitmap.Config.ARGB_8888);
        animated.setBounds(0, 0, 2, 2);
        animated.draw(new Canvas(target));
        Log.i(TAG, "AnimatedImageDrawable: repeat count " + animated.getRepeatCount() + ", first frame "
                + String.format("%08X", target.getPixel(0, 0)) + ", running " + animated.isRunning());
        animated.start();
        boolean started = animated.isRunning();
        animated.stop();
        Log.i(TAG, "AnimatedImageDrawable: running after start " + started + ", after stop " + animated.isRunning());
    }

    private static void mesh() {
        MeshSpecification.Attribute[] attributes = {
                new MeshSpecification.Attribute(MeshSpecification.TYPE_FLOAT2, 0, "position")};
        MeshSpecification.Varying[] varyings = {};
        String vertex = "Varyings main(const Attributes attributes) { Varyings varyings;"
                + " varyings.position = attributes.position; return varyings; }";
        String fragment = "float2 main(const Varyings varyings, out float4 color) {"
                + " color = float4(0.0, 0.5, 1.0, 1.0); return varyings.position; }";
        MeshSpecification spec = MeshSpecification.make(attributes, 8, varyings, vertex, fragment);
        float[] triangle = {0f, 0f, 8f, 0f, 0f, 8f};
        FloatBuffer vertices = ByteBuffer.allocateDirect(triangle.length * 4).order(ByteOrder.nativeOrder())
                .asFloatBuffer();
        vertices.put(triangle);
        vertices.rewind();
        Mesh mesh = new Mesh(spec, Mesh.TRIANGLES, vertices, 3, new RectF(0f, 0f, 8f, 8f));
        Log.i(TAG, "Mesh: made " + (spec != null && mesh != null));
        // A Bitmap's canvas is a software one, where Android's BaseCanvas.drawMesh throws; a hardware canvas draws
        // the mesh.
        Bitmap target = Bitmap.createBitmap(8, 8, Bitmap.Config.ARGB_8888);
        target.eraseColor(Color.WHITE);
        Paint paint = new Paint();
        paint.setColor(Color.WHITE);
        new Canvas(target).drawMesh(mesh, BlendMode.DST, paint);
        Log.i(TAG, "Mesh: inside " + String.format("%08X", target.getPixel(1, 1)) + ", outside "
                + String.format("%08X", target.getPixel(7, 7)));
    }

    private static void part(String name, ThrowingRunnable body) {
        try {
            body.run();
        } catch (Throwable e) {
            Log.i(TAG, name + ": " + e.getClass().getName());
            Log.i(TAG + "-device", name + ": " + e.getMessage());
        }
    }

    interface ThrowingRunnable {
        void run() throws Exception;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        ByteArrayOutputStream png = new ByteArrayOutputStream();
        image().compress(Bitmap.CompressFormat.PNG, 100, png);
        byte[] bytes = png.toByteArray();
        part("BitmapRegionDecoder", () -> regionDecoder(bytes));
        part("Gainmap", GraphicsClassesApplication::gainmap);
        part("AnimatedImageDrawable", GraphicsClassesApplication::animatedImage);
        part("Mesh", GraphicsClassesApplication::mesh);
        Log.i(TAG, "done");
    }
}
