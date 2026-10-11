package io.github.kisame76.minato.test.cxxbinding;

import android.app.Application;
import android.util.Log;

/**
 * The self-quit crash's test: loads its JNI library, which links the NDK's static C++ library as Unity's libil2cpp.so
 * does, and logs which image the dynamic linker bound its references to the C++ runtime to, then a C++ exception
 * thrown and caught on a thread of the library's own.
 */
public class CxxBindingApplication extends Application {
    static final String TAG = "cxx-binding-test";

    private static native String bindings();

    private static native String throwOnThread();

    @Override
    public void onCreate() {
        super.onCreate();
        System.loadLibrary("cxx-binding-test");
        Log.i(TAG, "bindings: " + bindings());
        Log.i(TAG, "on a thread: " + throwOnThread());
        Log.i(TAG, "done");
    }
}
