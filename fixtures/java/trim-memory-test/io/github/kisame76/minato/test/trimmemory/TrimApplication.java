package io.github.kisame76.minato.test.trimmemory;

import android.app.Application;
import android.content.ComponentCallbacks2;
import android.content.res.Configuration;
import android.util.Log;

/**
 * The Application of trim-memory-test: logs the memory callbacks it gets, and registers a callback of its own for the
 * Application's context (the order the system calls them in is part of the test).
 */
public class TrimApplication extends Application {
    static final String TAG = "trim-memory-test";
    /** The tag of the lines the script waits for and the reference leaves out. */
    static final String STEPS = "trim-memory-test-steps";
    /** The system trims a new process once at its start (level 40 on Android 15), before the window has the focus. */
    static volatile boolean ready;

    /** A memory callback's line, once the window has the focus. */
    static void memory(String line) {
        if (ready) {
            Log.i(TAG, line);
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        Log.i(TAG, "application onCreate");
        registerComponentCallbacks(new ComponentCallbacks2() {
            @Override
            public void onTrimMemory(int level) {
                memory("registered onTrimMemory " + level);
            }

            @Override
            public void onConfigurationChanged(Configuration newConfig) {
            }

            @Override
            public void onLowMemory() {
                memory("registered onLowMemory");
            }
        });
    }

    @Override
    public void onTrimMemory(int level) {
        super.onTrimMemory(level);
        memory("application onTrimMemory " + level);
    }

    @Override
    public void onLowMemory() {
        super.onLowMemory();
        memory("application onLowMemory");
    }
}
