package io.github.kisame76.minato.test.glsurfacereturns;

import android.app.Activity;
import android.opengl.GLES20;
import android.opengl.GLSurfaceView;
import android.os.Bundle;
import android.util.Log;
import android.view.SurfaceHolder;

import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

/**
 * A GLSurfaceView with ES 2.0, RGBA 8888 and a 24-bit depth buffer that draws continuously (RENDERMODE_CONTINUOUSLY)
 * through four rounds of HOME and the return, each of which gives it a new surface. It logs with the tag
 * glsurface-returns-test, on the main thread only, so that the order is the main thread's: the activity's callbacks,
 * its holder's (surfaceCreated, surfaceChanged, surfaceDestroyed), and "surface <n>: 30 frames" once the renderer drew
 * 30 frames on its holder's n-th surface, which the host's next HOME waits for; a pause that keeps the surface, such as
 * a dialog of another task over it, gives the renderer a new EGL surface but no new line. After the fifth surface's
 * line, what the renderer saw, "done", and its finish.
 */
public class GlReturnsActivity extends Activity {
    private static final String TAG = "glsurface-returns-test";
    /** The surfaces: the start's and one per return. */
    private static final int SURFACES = 5;

    private GLSurfaceView mView;
    private final Object mLock = new Object();
    private int mSurfacesCreated, mSurfacesChanged, mFrames, mFramesOnSurface, mSurfacesDrawn;
    /** The holder's surfaces so far, on the main thread. */
    private int mHolderSurfaces;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Log.i(TAG, "onCreate");
        mView = new GLSurfaceView(this);
        mView.setEGLContextClientVersion(2);
        mView.setEGLConfigChooser(8, 8, 8, 8, 24, 0);
        mView.setRenderer(new Renderer());
        mView.setRenderMode(GLSurfaceView.RENDERMODE_CONTINUOUSLY);
        mView.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                mHolderSurfaces++;
                Log.i(TAG, "surfaceCreated");
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                Log.i(TAG, "surfaceChanged");
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                Log.i(TAG, "surfaceDestroyed");
            }
        });
        setContentView(mView);
    }

    @Override
    protected void onStart() {
        super.onStart();
        Log.i(TAG, "onStart");
    }

    @Override
    protected void onRestart() {
        super.onRestart();
        Log.i(TAG, "onRestart");
    }

    @Override
    protected void onResume() {
        super.onResume();
        Log.i(TAG, "onResume");
        mView.onResume();
    }

    @Override
    protected void onPause() {
        Log.i(TAG, "onPause");
        mView.onPause();
        super.onPause();
    }

    @Override
    protected void onStop() {
        super.onStop();
        Log.i(TAG, "onStop");
    }

    /** On the main thread: the renderer drew 30 frames on its newest EGL surface. */
    private void drewThirty() {
        int surface;
        synchronized (mLock) {
            if (mSurfacesDrawn >= mHolderSurfaces) return;
            surface = ++mSurfacesDrawn;
        }
        Log.i(TAG, "surface " + surface + ": 30 frames");
        if (surface < SURFACES) return;
        synchronized (mLock) {
            Log.i(TAG, "renderer: onSurfaceCreated at least " + SURFACES + " times " + (mSurfacesCreated >= SURFACES)
                    + ", onSurfaceChanged at least " + SURFACES + " times " + (mSurfacesChanged >= SURFACES)
                    + ", frames more than " + 30 * SURFACES + " " + (mFrames > 30 * SURFACES));
        }
        Log.i(TAG, "done");
        finish();
    }

    private final class Renderer implements GLSurfaceView.Renderer {
        @Override
        public void onSurfaceCreated(GL10 unused, EGLConfig config) {
            synchronized (mLock) {
                mSurfacesCreated++;
                mFramesOnSurface = 0;
            }
        }

        @Override
        public void onSurfaceChanged(GL10 unused, int width, int height) {
            GLES20.glViewport(0, 0, width, height);
            synchronized (mLock) {
                mSurfacesChanged++;
                mFramesOnSurface = 0;
            }
        }

        @Override
        public void onDrawFrame(GL10 unused) {
            int onSurface;
            synchronized (mLock) {
                mFrames++;
                onSurface = ++mFramesOnSurface;
            }
            GLES20.glClearColor((onSurface % 60) / 60f, 0.25f, 0.5f, 1f);
            GLES20.glClearDepthf(1f);
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT | GLES20.GL_DEPTH_BUFFER_BIT);
            if (onSurface == 30) runOnUiThread(GlReturnsActivity.this::drewThirty);
        }
    }
}
