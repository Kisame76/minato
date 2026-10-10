package io.github.kisame76.minato.test.surface;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Rect;
import android.opengl.GLES20;
import android.opengl.GLES30;
import android.opengl.GLSurfaceView;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Choreographer;
import android.view.Gravity;
import android.view.PixelCopy;
import android.view.SurfaceHolder;
import android.view.ViewGroup;
import android.view.ViewTreeObserver;
import android.widget.Button;
import android.widget.FrameLayout;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.util.ArrayList;
import java.util.List;

import javax.microedition.khronos.egl.EGL10;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.egl.EGLDisplay;
import javax.microedition.khronos.opengles.GL10;

/**
 * ADR-0043 step c4's test (ADR-0041 test 1 without touches): a full-screen GLSurfaceView with ES 3.0 that draws three
 * fixed frames, each when asked, and reads each back with glReadPixels. The holder's callbacks, with their format and
 * size, are logged as they come on the main thread; then, at the window's first focus, five Choreographer frames, the
 * frames asked one after another, the view's bounds, and the renderer's callbacks and pixels, which come on the GL
 * thread and are logged in their own order at the end, so that the log does not depend on how the threads interleave.
 * A frame the GL thread draws again, as a redraw the system asks for, is logged once; the number of draws goes to the
 * tag surface-test-device, which the check shows and does not compare, as does the GL driver's name. Then a Button
 * comes over the GLSurfaceView, centered (ADR-0043 test 4: the window's layer, hidden over the hole, shows again):
 * its place is logged once it is laid out, and the end at its first draw. At surfaceCreated, before the GL thread can
 * draw (it waits for surfaceChanged's size), PixelCopy copies the surface through request(SurfaceView) and the Request
 * API, and refuses an immutable bitmap: Android answers ERROR_SOURCE_NO_DATA for a surface without a queued frame
 * (the Unity test APK probe, Unity's pause path); the answers, with the thread the listeners ran on, come before the view's bounds.
 */
public class SurfaceActivity extends Activity {
    private static final String TAG = "surface-test";
    private static final String DEVICE_TAG = "surface-test-device";
    private static final int FRAMES = 3;
    private static final int CHOREOGRAPHER_FRAMES = 5;

    private GLSurfaceView mView;
    private final FixedRenderer mRenderer = new FixedRenderer();
    private boolean mStarted;
    private boolean mChoreographerDone;
    private int mAsked;
    private int mDrawn = -1;
    private int mTicks;
    private long mLastTick;
    private boolean mIncreasing = true;
    private final List<String> mCopies = new ArrayList<>();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mView = new GLSurfaceView(this);
        mView.setEGLContextClientVersion(3);
        mView.setRenderer(mRenderer);
        mView.setRenderMode(GLSurfaceView.RENDERMODE_WHEN_DIRTY);
        mView.getHolder().addCallback(new SurfaceHolder.Callback2() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                Rect frame = holder.getSurfaceFrame();
                Log.i(TAG, "surface created: valid " + holder.getSurface().isValid() + ", frame " + frame.width()
                        + " x " + frame.height());
                copy();
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                Log.i(TAG, "surface changed: format " + format + ", " + width + " x " + height);
            }

            @Override
            public void surfaceRedrawNeeded(SurfaceHolder holder) {
                Log.i(TAG, "surface redraw needed");
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                Log.i(TAG, "surface destroyed");
            }
        });
        setContentView(mView);
        Log.i(TAG, "created: render mode " + mView.getRenderMode());
    }

    @Override
    protected void onResume() {
        super.onResume();
        mView.onResume();
        Log.i(TAG, "resumed");
    }

    @Override
    protected void onPause() {
        mView.onPause();
        super.onPause();
        Log.i(TAG, "paused");
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!hasFocus || mStarted) {
            return;
        }
        mStarted = true;
        Log.i(TAG, "focus");
        Choreographer.getInstance().postFrameCallback(new Choreographer.FrameCallback() {
            @Override
            public void doFrame(long frameTimeNanos) {
                mIncreasing &= frameTimeNanos > mLastTick;
                mLastTick = frameTimeNanos;
                if (++mTicks < CHOREOGRAPHER_FRAMES) {
                    Choreographer.getInstance().postFrameCallback(this);
                    return;
                }
                Log.i(TAG, "choreographer: " + mTicks + " frames, times increasing " + mIncreasing);
                mChoreographerDone = true;
                advance();
            }
        });
    }

    /** On the main thread: the GL thread drew `frame` for the first time. */
    void drawn(int frame) {
        mDrawn = frame;
        advance();
    }

    /** The next frame once the asked one is drawn, and the end after the last. */
    private void advance() {
        if (!mChoreographerDone || mDrawn < mAsked) {
            return;
        }
        if (mAsked < FRAMES - 1) {
            mRenderer.ask(++mAsked);
            mView.requestRender();
            return;
        }
        for (String line : mCopies) {
            Log.i(TAG, "pixel copy: " + line);
        }
        int[] location = new int[2];
        mView.getLocationInWindow(location);
        Log.i(TAG, "view: at " + location[0] + ", " + location[1] + ", " + mView.getWidth() + " x "
                + mView.getHeight());
        for (String line : mRenderer.lines()) {
            Log.i(TAG, "renderer: " + line);
        }
        Log.i(DEVICE_TAG, "draws " + mRenderer.draws() + ", " + mRenderer.driver());
        addButton();
    }

    /** PixelCopy of the new surface, which has no frame yet: its two ways, and an immutable bitmap it refuses. */
    private void copy() {
        Bitmap dest = Bitmap.createBitmap(4, 4, Bitmap.Config.ARGB_8888);
        PixelCopy.request(mView, dest, result -> mCopies.add("request(SurfaceView): " + result + ", on the main thread "
                + (Looper.myLooper() == Looper.getMainLooper())), new Handler(Looper.getMainLooper()));
        PixelCopy.Request request = PixelCopy.Request.Builder.ofSurface(mView)
                .setDestinationBitmap(Bitmap.createBitmap(4, 4, Bitmap.Config.ARGB_8888)).build();
        PixelCopy.request(request, getMainExecutor(), result -> {
            String bitmap;
            try {
                bitmap = "a bitmap " + (result.getBitmap() != null);
            } catch (RuntimeException e) {
                bitmap = e.getClass().getName() + ": " + e.getMessage();
            }
            mCopies.add("Request.ofSurface: " + result.getStatus() + ", " + bitmap + ", on the main thread "
                    + (Looper.myLooper() == Looper.getMainLooper()));
        });
        try {
            PixelCopy.request(mView, Bitmap.createBitmap(new int[16], 4, 4, Bitmap.Config.ARGB_8888),
                    result -> mCopies.add("immutable: " + result), new Handler(Looper.getMainLooper()));
            mCopies.add("immutable: asked");
        } catch (RuntimeException e) {
            mCopies.add("immutable: " + e.getClass().getName() + ": " + e.getMessage());
        }
    }

    /** A Button over the GLSurfaceView, centered: its place after its layout, and the end at its first draw. */
    private void addButton() {
        Button button = new Button(this) {
            private boolean mDrawn;

            @Override
            protected void onDraw(Canvas canvas) {
                super.onDraw(canvas);
                if (!mDrawn) {
                    mDrawn = true;
                    Log.i(TAG, "done");
                }
            }
        };
        button.setText("OK");
        addContentView(button, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER));
        button.getViewTreeObserver().addOnPreDrawListener(new ViewTreeObserver.OnPreDrawListener() {
            @Override
            public boolean onPreDraw() {
                button.getViewTreeObserver().removeOnPreDrawListener(this);
                int[] location = new int[2];
                button.getLocationInWindow(location);
                Log.i(TAG, "button: added at " + location[0] + ", " + location[1] + ", " + button.getWidth() + " x "
                        + button.getHeight());
                return true;
            }
        });
    }

    /** Draws the frame it was asked for: frame 0 a clear, frame 1 a clear and a scissored one, frame 2 a triangle. */
    final class FixedRenderer implements GLSurfaceView.Renderer {
        private final List<String> mLines = new ArrayList<>();
        private volatile int mFrame;
        private int mRecorded = -1;
        private int mDraws;
        private String mDriver = "";
        private int mWidth;
        private int mHeight;
        private int mProgram;

        void ask(int frame) {
            mFrame = frame;
        }

        synchronized List<String> lines() {
            return new ArrayList<>(mLines);
        }

        synchronized int draws() {
            return mDraws;
        }

        synchronized String driver() {
            return mDriver;
        }

        private synchronized void record(String line) {
            mLines.add(line);
        }

        @Override
        public void onSurfaceCreated(GL10 gl, EGLConfig config) {
            EGL10 egl = (EGL10) javax.microedition.khronos.egl.EGLContext.getEGL();
            EGLDisplay display = egl.eglGetCurrentDisplay();
            int[] value = new int[1];
            StringBuilder bits = new StringBuilder();
            int[] attributes = {EGL10.EGL_RED_SIZE, EGL10.EGL_GREEN_SIZE, EGL10.EGL_BLUE_SIZE, EGL10.EGL_ALPHA_SIZE};
            for (int attribute : attributes) {
                egl.eglGetConfigAttrib(display, config, attribute, value);
                bits.append(bits.length() == 0 ? "" : " ").append(value[0]);
            }
            int[] major = new int[1];
            GLES30.glGetIntegerv(GLES30.GL_MAJOR_VERSION, major, 0);
            record("surface created: ES " + major[0] + ", config rgba " + bits);
            synchronized (this) {
                mDriver = GLES20.glGetString(GLES20.GL_RENDERER);
            }
            mProgram = program();
        }

        @Override
        public void onSurfaceChanged(GL10 gl, int width, int height) {
            mWidth = width;
            mHeight = height;
            GLES20.glViewport(0, 0, width, height);
            record("surface changed: " + width + " x " + height);
        }

        @Override
        public void onDrawFrame(GL10 gl) {
            int frame = mFrame;
            synchronized (this) {
                mDraws++;
            }
            GLES20.glDisable(GLES20.GL_SCISSOR_TEST);
            if (frame == 0) {
                GLES20.glClearColor(64 / 255f, 128 / 255f, 192 / 255f, 1f);
                GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
            } else if (frame == 1) {
                GLES20.glClearColor(1f, 0f, 0f, 1f);
                GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
                GLES20.glEnable(GLES20.GL_SCISSOR_TEST);
                GLES20.glScissor(0, 0, mWidth / 2, mHeight);
                GLES20.glClearColor(0f, 1f, 0f, 1f);
                GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
                GLES20.glDisable(GLES20.GL_SCISSOR_TEST);
            } else {
                GLES20.glClearColor(0f, 0f, 0f, 1f);
                GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
                triangle();
            }
            if (frame <= mRecorded) {
                return;
            }
            mRecorded = frame;
            record("frame " + frame + ": center " + pixel(mWidth / 2, mHeight / 2) + ", left " + pixel(mWidth / 4,
                    mHeight / 2) + ", right " + pixel(mWidth * 3 / 4, mHeight / 2) + ", error 0x"
                    + Integer.toHexString(GLES20.glGetError()));
            mView.post(() -> drawn(frame));
        }

        private String pixel(int x, int y) {
            ByteBuffer rgba = ByteBuffer.allocateDirect(4).order(ByteOrder.nativeOrder());
            GLES20.glReadPixels(x, y, 1, 1, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, rgba);
            return String.format("%02x%02x%02x%02x", rgba.get(0) & 0xff, rgba.get(1) & 0xff, rgba.get(2) & 0xff,
                    rgba.get(3) & 0xff);
        }

        /** A triangle over the center in a color of exact bytes, through an ES 3.0 shader. */
        private void triangle() {
            float[] corners = {-0.5f, -0.5f, 0.5f, -0.5f, 0f, 0.5f};
            FloatBuffer vertices = ByteBuffer.allocateDirect(corners.length * 4).order(ByteOrder.nativeOrder())
                    .asFloatBuffer();
            vertices.put(corners).position(0);
            GLES20.glUseProgram(mProgram);
            GLES20.glVertexAttribPointer(0, 2, GLES20.GL_FLOAT, false, 0, vertices);
            GLES20.glEnableVertexAttribArray(0);
            GLES20.glDrawArrays(GLES20.GL_TRIANGLES, 0, 3);
            GLES20.glDisableVertexAttribArray(0);
        }

        private int program() {
            int vertex = shader(GLES20.GL_VERTEX_SHADER, "#version 300 es\n"
                    + "layout(location = 0) in vec2 position;\n"
                    + "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n");
            int fragment = shader(GLES20.GL_FRAGMENT_SHADER, "#version 300 es\n"
                    + "precision highp float;\n"
                    + "out vec4 color;\n"
                    + "void main() { color = vec4(1.0, 64.0 / 255.0, 0.0, 1.0); }\n");
            int program = GLES20.glCreateProgram();
            GLES20.glAttachShader(program, vertex);
            GLES20.glAttachShader(program, fragment);
            GLES20.glLinkProgram(program);
            int[] linked = new int[1];
            GLES20.glGetProgramiv(program, GLES20.GL_LINK_STATUS, linked, 0);
            record("program linked " + (linked[0] != 0));
            return program;
        }

        private int shader(int type, String source) {
            int shader = GLES20.glCreateShader(type);
            GLES20.glShaderSource(shader, source);
            GLES20.glCompileShader(shader);
            int[] compiled = new int[1];
            GLES20.glGetShaderiv(shader, GLES20.GL_COMPILE_STATUS, compiled, 0);
            if (compiled[0] == 0) {
                record("shader " + type + " failed: " + GLES20.glGetShaderInfoLog(shader));
            }
            return shader;
        }
    }
}
