package io.github.kisame76.minato.test.glsurfacepause;

import android.app.Activity;
import android.opengl.GLES20;
import android.opengl.GLSurfaceView;
import android.os.Bundle;
import android.os.SystemClock;
import android.util.Log;
import android.view.SurfaceHolder;

import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

/**
 * A GLSurfaceView with ES 2.0 that draws continuously (RENDERMODE_CONTINUOUSLY, libGDX's way) through HOME and the
 * return. It logs with the tag glsurface-pause-test, on the main thread only, so that the order is the main thread's:
 * the activity's callbacks, its holder's (surfaceCreated, surfaceChanged, surfaceDestroyed), "ready" once the renderer
 * drew 30 frames on its first surface, whether GLSurfaceView.onPause returned (it waits for its GL thread to pause),
 * "frames after the return" once the renderer drew 30 frames on its second surface, then what the renderer saw, and
 * "done". How long onPause took goes to the tag with "-device".
 */
public class GlPauseActivity extends Activity {
    private static final String TAG = "glsurface-pause-test";

    private GLSurfaceView mView;
    private final Object mLock = new Object();
    private int mSurfacesCreated, mSurfacesChanged, mFrames, mFramesOnSurface;
    private boolean mReady, mReturned, mDone;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Log.i(TAG, "onCreate");
        mView = new GLSurfaceView(this);
        mView.setEGLContextClientVersion(2);
        mView.setRenderer(new Renderer());
        mView.setRenderMode(GLSurfaceView.RENDERMODE_CONTINUOUSLY);
        mView.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
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
        final long start = SystemClock.uptimeMillis();
        mView.onPause();
        Log.i(TAG, "GLSurfaceView.onPause returned");
        Log.i(TAG + "-device", "GLSurfaceView.onPause took " + (SystemClock.uptimeMillis() - start) + " ms");
        synchronized (mLock) {
            if (mReady) mReturned = true;
        }
        super.onPause();
    }

    @Override
    protected void onStop() {
        super.onStop();
        Log.i(TAG, "onStop");
    }

    /** On the main thread: the renderer drew 30 frames on its first surface, or on one after the return. */
    private void drewThirty() {
        boolean ready, done;
        synchronized (mLock) {
            ready = !mReady && !mReturned;
            done = mReturned && !mDone;
            if (ready) mReady = true;
            if (done) mDone = true;
        }
        if (ready) Log.i(TAG, "ready");
        if (done) {
            Log.i(TAG, "frames after the return");
            synchronized (mLock) {
                Log.i(TAG, "renderer: onSurfaceCreated " + mSurfacesCreated + " times, onSurfaceChanged at least "
                        + "twice " + (mSurfacesChanged >= 2) + ", frames " + (mFrames > 60 ? "more than 60" : mFrames));
            }
            Log.i(TAG, "done");
            finish();
        }
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
            GLES20.glClear(GLES20.GL_COLOR_BUFFER_BIT);
            if (onSurface == 30) runOnUiThread(GlPauseActivity.this::drewThirty);
        }
    }
}
