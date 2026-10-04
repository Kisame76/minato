package io.github.kisame76.minato.test.windowsurface;

import android.app.Application;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.Point;
import android.graphics.Rect;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.view.Surface;
import android.view.WindowSurface;

import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.zip.CRC32;

/**
 * ADR-0043 c2's test of Surface's natives, on graphics-run's window 1 of 64 x 48 pixels (MINATO_WINDOW), which
 * tools/window-surface-check.sh runs with the Metal hook reading back the first 4 drawables presented and printing
 * each one's CRC-32 of its BGRA bytes (graphics/tests/metal_hook.m). Minato only: it takes its Surface from Minato's
 * WindowSurface, as ViewRootImpl takes the window's.
 * - a) WindowSurface.create on window 1, then Surface.transferFrom into a new Surface, as ViewRootImpl does;
 * - b) EGL14's window surface on it: its size, a clear read back, and eglSwapBuffers' drawable; then
 *   eglDestroySurface, after which Android lets the CPU lock the window. EGL14's public eglCreateWindowSurface first
 *   asks whether its argument is a SurfaceView, a compile-only stub until ADR-0043 c4, so the test calls the binding's
 *   native behind it, _eglCreateWindowSurface, which takes the Surface's window through minato_surface_window;
 * - c) lockCanvas without a dirty rectangle: the canvas, two fills, and unlockCanvasAndPost's drawable, whose CRC-32
 *   the app computes from the same drawing on a Bitmap;
 * - d) lockCanvas with a dirty rectangle: the rectangle as the host answers it, the canvas's clip, and a drawable in
 *   which only that rectangle changed;
 * - e) WindowSurface.setBuffersGeometry: a size with one 0 refused, then 32 x 24 RGBA_8888 for the next canvas and
 *   its drawable;
 * - f) its JNI library: ANativeWindow_fromSurface and ANativeWindow_toSurface;
 * - g) release, after which the reference that fromSurface took still holds the window.
 * The fills are opaque colors of channels 0 and 255, exact in every format. Each line goes to System.out with the
 * prefix "window-surface-test: "; the last is "done".
 */
public class WindowSurfaceApplication extends Application {
    private static final String PREFIX = "window-surface-test: ";
    private static final int WIDTH = 64;
    private static final int HEIGHT = 48;

    @Override
    public void onCreate() {
        super.onCreate();
        try {
            System.loadLibrary("window-surface-test");
            run();
        } catch (Throwable t) {
            print("failed: " + t);
        }
        print("done");
    }

    private static void print(String line) {
        System.out.println(PREFIX + line);
    }

    private static void run() throws ReflectiveOperationException {
        Surface created = WindowSurface.create(1, WIDTH, HEIGHT);
        print("a) create: valid " + created.isValid() + ", " + size(created));
        Surface surface = new Surface();
        surface.transferFrom(created);
        print("a) transferFrom into a new Surface: valid " + surface.isValid() + ", " + size(surface)
                + "; the old one valid " + created.isValid());

        egl(surface);

        Bitmap model = Bitmap.createBitmap(WIDTH, HEIGHT, Bitmap.Config.ARGB_8888);
        Canvas canvas = surface.lockCanvas(null);
        print("c) lockCanvas: " + canvas.getWidth() + " x " + canvas.getHeight() + ", hardware accelerated "
                + canvas.isHardwareAccelerated());
        fill(canvas, Color.RED, Color.GREEN);
        fill(new Canvas(model), Color.RED, Color.GREEN);
        surface.unlockCanvasAndPost(canvas);
        print("c) the drawable must have CRC-32 " + crc(model));

        Rect dirty = new Rect(8, 8, 24, 24);
        canvas = surface.lockCanvas(dirty);
        print("d) lockCanvas(8, 8, 24, 24): dirty " + dirty.toShortString() + ", clip "
                + canvas.getClipBounds().toShortString());
        canvas.drawColor(Color.BLUE);
        Canvas modelCanvas = new Canvas(model);
        modelCanvas.clipRect(dirty);
        modelCanvas.drawColor(Color.BLUE);
        surface.unlockCanvasAndPost(canvas);
        print("d) the drawable must have CRC-32 " + crc(model));

        int refused = WindowSurface.setBuffersGeometry(surface, 0, 24, PixelFormat.RGBA_8888);
        int taken = WindowSurface.setBuffersGeometry(surface, 32, 24, PixelFormat.RGBA_8888);
        Bitmap small = Bitmap.createBitmap(32, 24, Bitmap.Config.ARGB_8888);
        canvas = surface.lockCanvas(null);
        print("e) setBuffersGeometry (0, 24, RGBA_8888) " + refused + ", (32, 24, RGBA_8888) " + taken
                + "; lockCanvas: " + canvas.getWidth() + " x " + canvas.getHeight());
        fill(canvas, Color.YELLOW, Color.CYAN);
        fill(new Canvas(small), Color.YELLOW, Color.CYAN);
        surface.unlockCanvasAndPost(canvas);
        print("e) the drawable must have CRC-32 " + crc(small));

        print("f) ANativeWindow_fromSurface: " + nativeFromSurface(surface));
        Surface back = nativeToSurface();
        print("f) ANativeWindow_toSurface: valid " + back.isValid() + ", " + size(back));
        back.release();

        surface.release();
        print("g) release: valid " + surface.isValid() + "; the window of fromSurface's reference: " + nativeKept());
    }

    /** EGL14's window surface on the Surface: a clear to blue, read back and swapped, then destroyed. */
    private static void egl(Surface surface) throws ReflectiveOperationException {
        EGLDisplay display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        int[] version = new int[2];
        EGL14.eglInitialize(display, version, 0, version, 1);
        int[] spec = {EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_ALPHA_SIZE, 8,
                EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT,
                EGL14.EGL_NONE};
        EGLConfig[] configs = new EGLConfig[1];
        int[] count = new int[1];
        boolean chosen = EGL14.eglChooseConfig(display, spec, 0, configs, 0, 1, count, 0);
        EGLContext context = EGL14.eglCreateContext(display, configs[0], EGL14.EGL_NO_CONTEXT,
                new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
        Method create = EGL14.class.getDeclaredMethod("_eglCreateWindowSurface", EGLDisplay.class, EGLConfig.class,
                Object.class, int[].class, int.class);
        create.setAccessible(true);
        EGLSurface window = (EGLSurface) create.invoke(null, display, configs[0], surface,
                new int[] {EGL14.EGL_NONE}, 0);
        int[] width = new int[1];
        int[] height = new int[1];
        EGL14.eglQuerySurface(display, window, EGL14.EGL_WIDTH, width, 0);
        EGL14.eglQuerySurface(display, window, EGL14.EGL_HEIGHT, height, 0);
        boolean current = EGL14.eglMakeCurrent(display, window, window, context);
        print("b) EGL14: a window config " + (chosen && count[0] == 1) + ", a window surface "
                + (window != EGL14.EGL_NO_SURFACE) + " of " + width[0] + " x " + height[0] + ", current " + current);
        GLES20.glViewport(0, 0, WIDTH, HEIGHT);
        GLES20.glClearColor(0f, 0f, 1f, 1f);
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
        ByteBuffer rgba = ByteBuffer.allocateDirect(4).order(ByteOrder.nativeOrder());
        GLES20.glReadPixels(32, 24, 1, 1, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, rgba);
        boolean swapped = EGL14.eglSwapBuffers(display, window);
        print("b) clear (32, 24) " + (rgba.get(0) & 0xff) + " " + (rgba.get(1) & 0xff) + " " + (rgba.get(2) & 0xff)
                + " " + (rgba.get(3) & 0xff) + ", eglSwapBuffers " + swapped + ", glGetError " + GLES20.glGetError());
        Bitmap blue = Bitmap.createBitmap(WIDTH, HEIGHT, Bitmap.Config.ARGB_8888);
        blue.eraseColor(Color.BLUE);
        print("b) the drawable must have CRC-32 " + crc(blue));
        boolean released = EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE,
                EGL14.EGL_NO_CONTEXT);
        print("b) released " + released + ", destroyed " + EGL14.eglDestroySurface(display, window) + " "
                + EGL14.eglDestroyContext(display, context) + ", eglTerminate " + EGL14.eglTerminate(display));
    }

    /** The left half in one color, the right half in another, as whole pixels. */
    private static void fill(Canvas canvas, int left, int right) {
        Paint paint = new Paint();
        paint.setColor(right);
        canvas.drawColor(left);
        canvas.drawRect(canvas.getWidth() / 2, 0, canvas.getWidth(), canvas.getHeight(), paint);
    }

    private static String size(Surface surface) {
        Point size = surface.getDefaultSize();
        return size.x + " x " + size.y;
    }

    /** The CRC-32 of the bitmap's pixels as a drawable holds them: B, G, R, A, row by row. */
    private static String crc(Bitmap bitmap) {
        byte[] bgra = new byte[bitmap.getWidth() * bitmap.getHeight() * 4];
        int index = 0;
        for (int y = 0; y < bitmap.getHeight(); y++) {
            for (int x = 0; x < bitmap.getWidth(); x++) {
                int color = bitmap.getPixel(x, y);
                bgra[index++] = (byte) Color.blue(color);
                bgra[index++] = (byte) Color.green(color);
                bgra[index++] = (byte) Color.red(color);
                bgra[index++] = (byte) Color.alpha(color);
            }
        }
        CRC32 crc = new CRC32();
        crc.update(bgra);
        return String.format("%08x", crc.getValue());
    }

    /** jni/window_surface_test.c: the window of ANativeWindow_fromSurface, kept with its reference. */
    private static native String nativeFromSurface(Surface surface);

    /** jni/window_surface_test.c: ANativeWindow_toSurface of the kept window. */
    private static native Surface nativeToSurface();

    /** jni/window_surface_test.c: the kept window's size, then its reference released. */
    private static native String nativeKept();
}
