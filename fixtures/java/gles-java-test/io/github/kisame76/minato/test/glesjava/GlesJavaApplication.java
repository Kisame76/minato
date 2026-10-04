package io.github.kisame76.minato.test.glesjava;

import android.app.Application;
import android.graphics.Bitmap;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.opengl.GLES30;
import android.opengl.GLUtils;
import android.util.Log;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;

import javax.microedition.khronos.egl.EGL10;
import javax.microedition.khronos.egl.EGLContext;
import javax.microedition.khronos.opengles.GL10;
import javax.microedition.khronos.opengles.GL11;

/**
 * ADR-0041 a)'s test of GLES from Java (decision 4; test 2's gap 8): AOSP's android.opengl,
 * javax.microedition.khronos and com.google.android.gles_jni with their core/jni natives over Minato's front libs.
 * It runs in Application.onCreate on the main thread, as there is no window before step c2c3, so every surface is a
 * 64 x 64 pbuffer:
 * - EGL10 as Godot's GLSurfaceView (its EglHelper, RegularConfigChooser and RegularContextFactory) uses it: getEGL,
 *   the default display, Godot's config spec (red, green and blue of 4 bits or more, the ES 2 bit) with its choice of
 *   8-bit red, green, blue and alpha and 16 bits of depth or more, a context of client version 3 asked with the debug
 *   flag first, getGL, a draw through GLES30, eglSwapBuffers, the destroys and eglTerminate;
 * - EGL14 with GLES20: the same draw with an ES 2.0 context;
 * - GL10 through GLImpl on an ES 1.1 context: GL_VERSION's profile, a clear and a fixed-function triangle whose array
 *   goes through glVertexPointerBounds;
 * - GLUtils (ADR-0041 a3) on an ES 2.0 context: getInternalFormat and getType of a 4 x 4 ARGB_8888 bitmap,
 *   texImage2D of it into a texture and texSubImage2D of a 2 x 2 one, the texels read back through a framebuffer;
 * - its JNI library (jni/, ADR-0040 decision 3: the graphics front libs are public libraries): loaded through the
 *   class loader's namespace, which must reach libEGL, libGLESv2 and libGLESv3, which it links as Godot's
 *   libgodot_android.so does; each front lib opened by its name, as Godot's volk opens libvulkan.so; and a clear from
 *   native code in an ES 2.0 context that EGL14 made current, read back there and from Java.
 * Each draw clears to (0.2, 0.4, 0.6, 1) and covers the lower left half with one color of channels 0 and 1, exact at
 * any precision; the pixels read back at pixel centers away from the edge are exact in 8 bits (ES 3.0.6's rules, as
 * ADR-0029's tests have them). It logs with the tag gles-java-test and ends
 * with "done"; what differs by device, the strings of the renderer and version, goes to gles-java-test-device.
 * tools/android-reference.sh compares it with Android 15's log.
 */
public class GlesJavaApplication extends Application {
    private static final String TAG = "gles-java-test";
    private static final int SIZE = 64;
    private static final int EGL_CONTEXT_CLIENT_VERSION = 0x3098;
    private static final int EGL_CONTEXT_FLAGS_KHR = 0x30FC;
    private static final int EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR = 0x1;
    private static final int EGL_OPENGL_ES_BIT = 0x1;
    private static final int EGL_OPENGL_ES2_BIT = 0x4;
    /** The lower left half of the viewport, below the diagonal from (-1, 1) to (1, -1). */
    private static final float[] TRIANGLE = {-1f, -1f, 1f, -1f, -1f, 1f};
    /** GLUtils' bitmap: red at the top left, green at the top right, blue at the bottom left, white. */
    private static final int[] QUADRANTS = {0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff};

    @Override
    public void onCreate() {
        super.onCreate();
        try {
            egl10();
            egl14();
            gl10();
            glutils();
            jni();
        } catch (Throwable t) {
            Log.i(TAG, "failed: " + t);
            // Its stack, which differs by device, is only shown.
            Log.i(TAG + "-device", Log.getStackTraceString(t));
        }
        Log.i(TAG, "done");
    }

    /** EGL10 as Godot's GLSurfaceView uses it, then GLES30 on its context. */
    private void egl10() {
        EGL10 egl = (EGL10) EGLContext.getEGL();
        Log.i(TAG, "EGL10: getEGL " + egl.getClass().getName());
        javax.microedition.khronos.egl.EGLDisplay display = egl.eglGetDisplay(EGL10.EGL_DEFAULT_DISPLAY);
        boolean initialized = egl.eglInitialize(display, new int[2]);
        Log.i(TAG, "EGL10: a default display " + (display != EGL10.EGL_NO_DISPLAY) + ", initialized " + initialized);
        // RegularConfigChooser's spec, then its choice.
        int[] spec = {EGL10.EGL_RED_SIZE, 4, EGL10.EGL_GREEN_SIZE, 4, EGL10.EGL_BLUE_SIZE, 4, EGL10.EGL_RENDERABLE_TYPE,
                EGL_OPENGL_ES2_BIT, EGL10.EGL_NONE};
        int[] count = new int[1];
        egl.eglChooseConfig(display, spec, null, 0, count);
        javax.microedition.khronos.egl.EGLConfig[] configs = new javax.microedition.khronos.egl.EGLConfig[count[0]];
        egl.eglChooseConfig(display, spec, configs, count[0], count);
        javax.microedition.khronos.egl.EGLConfig chosen = null;
        for (javax.microedition.khronos.egl.EGLConfig config : configs) {
            if (attribute(egl, display, config, EGL10.EGL_RED_SIZE) == 8
                    && attribute(egl, display, config, EGL10.EGL_GREEN_SIZE) == 8
                    && attribute(egl, display, config, EGL10.EGL_BLUE_SIZE) == 8
                    && attribute(egl, display, config, EGL10.EGL_ALPHA_SIZE) == 8
                    && attribute(egl, display, config, EGL10.EGL_DEPTH_SIZE) >= 16
                    && (attribute(egl, display, config, EGL10.EGL_SURFACE_TYPE) & EGL10.EGL_PBUFFER_BIT) != 0) {
                chosen = config;
                break;
            }
        }
        Log.i(TAG, "EGL10: configs for Godot's spec " + (count[0] > 0) + ", one of 8-bit RGBA and 16-bit depth or more "
                + (chosen != null));
        // RegularContextFactory: the debug flag first, then without.
        EGLContext context = egl.eglCreateContext(display, chosen, EGL10.EGL_NO_CONTEXT,
                new int[] {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_CONTEXT_FLAGS_KHR, EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR,
                        EGL10.EGL_NONE});
        if (context == EGL10.EGL_NO_CONTEXT) {
            context = egl.eglCreateContext(display, chosen, EGL10.EGL_NO_CONTEXT,
                    new int[] {EGL_CONTEXT_CLIENT_VERSION, 3, EGL10.EGL_NONE});
        }
        javax.microedition.khronos.egl.EGLSurface surface = egl.eglCreatePbufferSurface(display, chosen,
                new int[] {EGL10.EGL_WIDTH, SIZE, EGL10.EGL_HEIGHT, SIZE, EGL10.EGL_NONE});
        boolean current = egl.eglMakeCurrent(display, surface, surface, context);
        Log.i(TAG, "EGL10: a context of client version 3 " + (context != EGL10.EGL_NO_CONTEXT) + ", a pbuffer "
                + (surface != EGL10.EGL_NO_SURFACE) + ", current " + current);
        GL10 gl = (GL10) context.getGL();
        Log.i(TAG, "EGL10: getGL " + gl.getClass().getName() + ", a GL11 " + (gl instanceof GL11));
        Log.i(TAG + "-device", "EGL10: GL_RENDERER " + GLES30.glGetString(GLES30.GL_RENDERER) + ", GL_VERSION "
                + GLES30.glGetString(GLES30.GL_VERSION));
        draw("GLES30 on EGL10's context", "#version 300 es\nin vec2 position;\n"
                + "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n", "#version 300 es\n"
                + "precision mediump float;\nout vec4 color;\nvoid main() { color = vec4(1.0, 0.0, 1.0, 1.0); }\n");
        boolean swapped = egl.eglSwapBuffers(display, surface);
        boolean released = egl.eglMakeCurrent(display, EGL10.EGL_NO_SURFACE, EGL10.EGL_NO_SURFACE,
                EGL10.EGL_NO_CONTEXT);
        Log.i(TAG, "EGL10: eglSwapBuffers on the pbuffer " + swapped + ", released " + released + ", destroyed "
                + egl.eglDestroySurface(display, surface) + " " + egl.eglDestroyContext(display, context)
                + ", eglTerminate " + egl.eglTerminate(display));
    }

    private static int attribute(EGL10 egl, javax.microedition.khronos.egl.EGLDisplay display,
            javax.microedition.khronos.egl.EGLConfig config, int name) {
        int[] value = new int[1];
        return egl.eglGetConfigAttrib(display, config, name, value) ? value[0] : 0;
    }

    /** EGL14 with an ES 2.0 context, then GLES20. */
    private void egl14() {
        EGLDisplay display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        int[] version = new int[2];
        boolean initialized = EGL14.eglInitialize(display, version, 0, version, 1);
        Log.i(TAG, "EGL14: a default display " + (display != EGL14.EGL_NO_DISPLAY) + ", initialized " + initialized);
        int[] spec = {EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_ALPHA_SIZE, 8,
                EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT,
                EGL14.EGL_NONE};
        EGLConfig[] configs = new EGLConfig[1];
        int[] count = new int[1];
        boolean chosen = EGL14.eglChooseConfig(display, spec, 0, configs, 0, 1, count, 0);
        android.opengl.EGLContext context = EGL14.eglCreateContext(display, configs[0], EGL14.EGL_NO_CONTEXT,
                new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
        EGLSurface surface = EGL14.eglCreatePbufferSurface(display, configs[0],
                new int[] {EGL14.EGL_WIDTH, SIZE, EGL14.EGL_HEIGHT, SIZE, EGL14.EGL_NONE}, 0);
        boolean current = EGL14.eglMakeCurrent(display, surface, surface, context);
        Log.i(TAG, "EGL14: a config of 8-bit RGBA " + (chosen && count[0] == 1) + ", an ES 2.0 context "
                + (context != EGL14.EGL_NO_CONTEXT) + ", a pbuffer " + (surface != EGL14.EGL_NO_SURFACE) + ", current "
                + current);
        draw("GLES20 on EGL14's context", "attribute vec2 position;\n"
                + "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n",
                "precision mediump float;\nvoid main() { gl_FragColor = vec4(0.0, 1.0, 1.0, 1.0); }\n");
        boolean released = EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE,
                EGL14.EGL_NO_CONTEXT);
        Log.i(TAG, "EGL14: released " + released + ", destroyed " + EGL14.eglDestroySurface(display, surface) + " "
                + EGL14.eglDestroyContext(display, context) + ", eglTerminate " + EGL14.eglTerminate(display));
    }

    /** A clear, then a triangle of one color with a shader program, read back inside and outside. */
    private static void draw(String what, String vertexShader, String fragmentShader) {
        GLES20.glViewport(0, 0, SIZE, SIZE);
        GLES20.glClearColor(0.2f, 0.4f, 0.6f, 1f);
        GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
        String clear = pixel(48, 48);
        int program = GLES20.glCreateProgram();
        GLES20.glAttachShader(program, shader(GLES20.GL_VERTEX_SHADER, vertexShader));
        GLES20.glAttachShader(program, shader(GLES20.GL_FRAGMENT_SHADER, fragmentShader));
        GLES20.glBindAttribLocation(program, 0, "position");
        GLES20.glLinkProgram(program);
        int[] linked = new int[1];
        GLES20.glGetProgramiv(program, GLES20.GL_LINK_STATUS, linked, 0);
        GLES20.glUseProgram(program);
        GLES20.glEnableVertexAttribArray(0);
        GLES20.glVertexAttribPointer(0, 2, GLES20.GL_FLOAT, false, 0, floats(TRIANGLE));
        GLES20.glDrawArrays(GLES20.GL_TRIANGLES, 0, 3);
        Log.i(TAG, what + ": linked " + (linked[0] == GLES20.GL_TRUE) + ", clear (48, 48) " + clear
                + ", triangle (16, 16) " + pixel(16, 16) + ", outside (48, 48) " + pixel(48, 48) + ", glGetError "
                + GLES20.glGetError());
        GLES20.glDeleteProgram(program);
    }

    private static int shader(int type, String source) {
        int shader = GLES20.glCreateShader(type);
        GLES20.glShaderSource(shader, source);
        GLES20.glCompileShader(shader);
        return shader;
    }

    /** The pixel whose center is (x + 0.5, y + 0.5), as "r g b a". */
    private static String pixel(int x, int y) {
        ByteBuffer rgba = ByteBuffer.allocateDirect(4).order(ByteOrder.nativeOrder());
        GLES20.glReadPixels(x, y, 1, 1, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, rgba);
        return (rgba.get(0) & 0xff) + " " + (rgba.get(1) & 0xff) + " " + (rgba.get(2) & 0xff) + " "
                + (rgba.get(3) & 0xff);
    }

    private static FloatBuffer floats(float[] values) {
        FloatBuffer buffer = ByteBuffer.allocateDirect(values.length * 4).order(ByteOrder.nativeOrder())
                .asFloatBuffer();
        buffer.put(values).position(0);
        return buffer;
    }

    /** GL10 through GLImpl on an ES 1.1 context of EGL10's. */
    private void gl10() {
        EGL10 egl = (EGL10) EGLContext.getEGL();
        javax.microedition.khronos.egl.EGLDisplay display = egl.eglGetDisplay(EGL10.EGL_DEFAULT_DISPLAY);
        egl.eglInitialize(display, new int[2]);
        int[] spec = {EGL10.EGL_RED_SIZE, 8, EGL10.EGL_GREEN_SIZE, 8, EGL10.EGL_BLUE_SIZE, 8, EGL10.EGL_ALPHA_SIZE, 8,
                EGL10.EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT, EGL10.EGL_SURFACE_TYPE, EGL10.EGL_PBUFFER_BIT,
                EGL10.EGL_NONE};
        javax.microedition.khronos.egl.EGLConfig[] configs = new javax.microedition.khronos.egl.EGLConfig[1];
        int[] count = new int[1];
        boolean chosen = egl.eglChooseConfig(display, spec, configs, 1, count) && count[0] == 1;
        EGLContext context = egl.eglCreateContext(display, configs[0], EGL10.EGL_NO_CONTEXT,
                new int[] {EGL_CONTEXT_CLIENT_VERSION, 1, EGL10.EGL_NONE});
        javax.microedition.khronos.egl.EGLSurface surface = egl.eglCreatePbufferSurface(display, configs[0],
                new int[] {EGL10.EGL_WIDTH, SIZE, EGL10.EGL_HEIGHT, SIZE, EGL10.EGL_NONE});
        boolean current = egl.eglMakeCurrent(display, surface, surface, context);
        GL10 gl = (GL10) context.getGL();
        String version = gl.glGetString(GL10.GL_VERSION);
        Log.i(TAG, "GL10: a config for ES 1.x " + chosen + ", an ES 1.1 context current " + current
                + ", GL_VERSION of the Common profile " + (version != null && version.startsWith("OpenGL ES-CM 1.1")));
        Log.i(TAG + "-device", "GL10: GL_VERSION " + version);
        gl.glViewport(0, 0, SIZE, SIZE);
        gl.glClearColor(0.2f, 0.4f, 0.6f, 1f);
        gl.glClear(GL10.GL_COLOR_BUFFER_BIT);
        gl.glMatrixMode(GL10.GL_PROJECTION);
        gl.glLoadIdentity();
        gl.glMatrixMode(GL10.GL_MODELVIEW);
        gl.glLoadIdentity();
        gl.glEnableClientState(GL10.GL_VERTEX_ARRAY);
        gl.glVertexPointer(2, GL10.GL_FLOAT, 0, floats(TRIANGLE));
        gl.glColor4f(1f, 1f, 0f, 1f);
        gl.glDrawArrays(GL10.GL_TRIANGLES, 0, 3);
        gl.glDisableClientState(GL10.GL_VERTEX_ARRAY);
        ByteBuffer rgba = ByteBuffer.allocateDirect(4).order(ByteOrder.nativeOrder());
        gl.glReadPixels(16, 16, 1, 1, GL10.GL_RGBA, GL10.GL_UNSIGNED_BYTE, rgba);
        String inside = (rgba.get(0) & 0xff) + " " + (rgba.get(1) & 0xff) + " " + (rgba.get(2) & 0xff) + " "
                + (rgba.get(3) & 0xff);
        gl.glReadPixels(48, 48, 1, 1, GL10.GL_RGBA, GL10.GL_UNSIGNED_BYTE, rgba);
        String outside = (rgba.get(0) & 0xff) + " " + (rgba.get(1) & 0xff) + " " + (rgba.get(2) & 0xff) + " "
                + (rgba.get(3) & 0xff);
        Log.i(TAG, "GL10: triangle (16, 16) " + inside + ", outside (48, 48) " + outside + ", glGetError "
                + gl.glGetError());
        egl.eglMakeCurrent(display, EGL10.EGL_NO_SURFACE, EGL10.EGL_NO_SURFACE, EGL10.EGL_NO_CONTEXT);
        Log.i(TAG, "GL10: destroyed " + egl.eglDestroySurface(display, surface) + " "
                + egl.eglDestroyContext(display, context) + ", eglTerminate " + egl.eglTerminate(display));
    }

    /** GLUtils' uploads of bitmaps on an ES 2.0 context, the texture read back through a framebuffer. */
    private void glutils() {
        EGLDisplay display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        int[] version = new int[2];
        EGL14.eglInitialize(display, version, 0, version, 1);
        int[] spec = {EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_ALPHA_SIZE, 8,
                EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT,
                EGL14.EGL_NONE};
        EGLConfig[] configs = new EGLConfig[1];
        int[] count = new int[1];
        EGL14.eglChooseConfig(display, spec, 0, configs, 0, 1, count, 0);
        android.opengl.EGLContext context = EGL14.eglCreateContext(display, configs[0], EGL14.EGL_NO_CONTEXT,
                new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
        EGLSurface surface = EGL14.eglCreatePbufferSurface(display, configs[0],
                new int[] {EGL14.EGL_WIDTH, SIZE, EGL14.EGL_HEIGHT, SIZE, EGL14.EGL_NONE}, 0);
        EGL14.eglMakeCurrent(display, surface, surface, context);
        Bitmap bitmap = Bitmap.createBitmap(4, 4, Bitmap.Config.ARGB_8888);
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                bitmap.setPixel(x, y, QUADRANTS[y / 2 * 2 + x / 2]);
            }
        }
        Log.i(TAG, "GLUtils: getInternalFormat " + GLUtils.getInternalFormat(bitmap) + ", getType "
                + GLUtils.getType(bitmap));
        int[] texture = new int[1];
        GLES20.glGenTextures(1, texture, 0);
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, texture[0]);
        GLUtils.texImage2D(GLES20.GL_TEXTURE_2D, 0, bitmap, 0);
        int[] framebuffer = new int[1];
        GLES20.glGenFramebuffers(1, framebuffer, 0);
        GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, framebuffer[0]);
        GLES20.glFramebufferTexture2D(GLES20.GL_FRAMEBUFFER, GLES20.GL_COLOR_ATTACHMENT0, GLES20.GL_TEXTURE_2D,
                texture[0], 0);
        boolean complete = GLES20.glCheckFramebufferStatus(GLES20.GL_FRAMEBUFFER) == GLES20.GL_FRAMEBUFFER_COMPLETE;
        Log.i(TAG, "GLUtils: texImage2D of the 4 x 4 bitmap, a complete framebuffer " + complete + ", texels "
                + texels() + ", glGetError " + GLES20.glGetError());
        Bitmap magenta = Bitmap.createBitmap(2, 2, Bitmap.Config.ARGB_8888);
        magenta.eraseColor(0xffff00ff);
        GLUtils.texSubImage2D(GLES20.GL_TEXTURE_2D, 0, 1, 1, magenta);
        Log.i(TAG, "GLUtils: texSubImage2D of a 2 x 2 magenta bitmap at (1, 1), texels " + texels() + ", glGetError "
                + GLES20.glGetError());
        GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, 0);
        GLES20.glDeleteFramebuffers(1, framebuffer, 0);
        GLES20.glDeleteTextures(1, texture, 0);
        EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
        EGL14.eglDestroySurface(display, surface);
        EGL14.eglDestroyContext(display, context);
        EGL14.eglTerminate(display);
    }

    /** The app's JNI library: loaded, the front libs opened by name, then a clear from native code in Java's context. */
    private void jni() {
        System.loadLibrary("gles-java-test");
        Log.i(TAG, "JNI: " + nativeOpen());
        EGLDisplay display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        int[] version = new int[2];
        EGL14.eglInitialize(display, version, 0, version, 1);
        int[] spec = {EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_ALPHA_SIZE, 8,
                EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT,
                EGL14.EGL_NONE};
        EGLConfig[] configs = new EGLConfig[1];
        int[] count = new int[1];
        EGL14.eglChooseConfig(display, spec, 0, configs, 0, 1, count, 0);
        android.opengl.EGLContext context = EGL14.eglCreateContext(display, configs[0], EGL14.EGL_NO_CONTEXT,
                new int[] {EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE}, 0);
        EGLSurface surface = EGL14.eglCreatePbufferSurface(display, configs[0],
                new int[] {EGL14.EGL_WIDTH, SIZE, EGL14.EGL_HEIGHT, SIZE, EGL14.EGL_NONE}, 0);
        EGL14.eglMakeCurrent(display, surface, surface, context);
        Log.i(TAG, "JNI: " + nativeDraw() + ", (32, 32) read from Java " + pixel(32, 32));
        EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
        EGL14.eglDestroySurface(display, surface);
        EGL14.eglDestroyContext(display, context);
        EGL14.eglTerminate(display);
    }

    /** jni/gles_java_test.c: which front libs dlopen opens by name, and libvulkan's vkGetInstanceProcAddr. */
    private static native String nativeOpen();

    /** jni/gles_java_test.c: a clear to magenta from native code in the current context, read back there. */
    private static native String nativeDraw();

    /** The framebuffer's 4 x 4 texels as "rrggbbaa", row by row from the texture's first, rows apart by " / ". */
    private static String texels() {
        ByteBuffer rgba = ByteBuffer.allocateDirect(4 * 16).order(ByteOrder.nativeOrder());
        GLES20.glReadPixels(0, 0, 4, 4, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, rgba);
        StringBuilder text = new StringBuilder();
        for (int index = 0; index < 64; ++index) {
            if (index > 0 && index % 4 == 0) {
                text.append(index % 16 == 0 ? " / " : " ");
            }
            text.append(Integer.toHexString(0x100 | (rgba.get(index) & 0xff)).substring(1));
        }
        return text.toString();
    }
}
