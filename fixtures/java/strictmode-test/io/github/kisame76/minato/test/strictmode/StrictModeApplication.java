package io.github.kisame76.minato.test.strictmode;

import android.app.Application;
import android.os.Build;
import android.os.Process;
import android.os.StrictMode;
import android.util.Log;
import java.net.InetAddress;

/**
 * ADR-0039 step c2d3's test, StrictMode at an app's start and the calls that reached the system's services. In its
 * onCreate, on the main thread, it logs under the tag strictmode-test: the thread and VM policies the bind gives the
 * app (StrictMode.initThreadDefaults and initVmDefaults: for target SDK 35, network on the main thread and file:// URIs
 * that leave the app are forbidden); Build.getSerial, which an app without READ_PRIVILEGED_PHONE_STATE may not read,
 * with the app's uid in its text written as "<uid>"; a name lookup on the main thread, which StrictMode stops before
 * any lookup; the policy of another thread, which the bind leaves alone; setVmPolicy with an empty policy, which asked
 * the network management service; enableDefaults; and the main thread's policy once it is LAX. A failing part logs
 * "failed" and its stack under strictmode-test-device, which differs by device and is only shown. The test needs no
 * window: as in drawables-test, the launcher activity is the framework's own android.app.Activity and the driver loads
 * only the Application class. tools/android-reference.sh takes the log from Android 15, and tools/strictmode-check.sh
 * compares Minato's with it.
 */
public class StrictModeApplication extends Application {
    private static final String TAG = "strictmode-test";

    /** One part of the test, which may throw. */
    private interface Part {
        void run() throws Exception;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        part("policies", StrictModeApplication::policies);
        part("serial", StrictModeApplication::serial);
        part("network on the main thread", StrictModeApplication::networkOnMainThread);
        part("another thread", StrictModeApplication::anotherThread);
        part("vm policy", StrictModeApplication::vmPolicy);
        part("defaults", StrictModeApplication::defaults);
        part("lax", StrictModeApplication::lax);
        Log.i(TAG, "done");
    }

    private static void part(String name, Part part) {
        try {
            part.run();
        } catch (Throwable t) {
            Log.i(TAG, name + ": failed: " + t);
            // Its stack, which differs by device, is only shown.
            Log.i(TAG + "-device", Log.getStackTraceString(t));
        }
    }

    /** The policies the bind gave the main thread and the process. */
    private static void policies() {
        Log.i(TAG, "policies: thread " + StrictMode.getThreadPolicy() + ", vm " + StrictMode.getVmPolicy());
    }

    /** Build.getSerial, which needs a privileged permission that no app holds. */
    private static void serial() {
        try {
            Log.i(TAG, "serial: " + Build.getSerial());
        } catch (SecurityException e) {
            String message = String.valueOf(e.getMessage()).replace(String.valueOf(Process.myUid()), "<uid>");
            Log.i(TAG, "serial: " + e.getClass().getName() + ": " + message);
        }
    }

    /** A name lookup, whose first step asks the thread's policy about network (Inet6AddressImpl.lookupHostByName). */
    private static void networkOnMainThread() {
        try {
            InetAddress.getByName("localhost");
            Log.i(TAG, "network on the main thread: looked up");
        } catch (Exception e) {
            Log.i(TAG, "network on the main thread: " + e.getClass().getName());
        }
    }

    /** The policy of a thread the app starts, which StrictMode keeps per thread. */
    private static void anotherThread() throws InterruptedException {
        String[] policy = {"none"};
        Thread thread = new Thread(() -> policy[0] = String.valueOf(StrictMode.getThreadPolicy()), "another");
        thread.start();
        thread.join(5000);
        Log.i(TAG, "another thread: " + policy[0]);
    }

    /** An empty VM policy, as apps set one to share file:// URIs again. */
    private static void vmPolicy() {
        StrictMode.setVmPolicy(new StrictMode.VmPolicy.Builder().build());
        Log.i(TAG, "vm policy: set, now " + StrictMode.getVmPolicy());
    }

    /** enableDefaults, as an app's debug build calls it, then both policies back to none. */
    private static void defaults() {
        StrictMode.enableDefaults();
        Log.i(TAG, "defaults: thread " + StrictMode.getThreadPolicy() + ", vm " + StrictMode.getVmPolicy());
        StrictMode.setThreadPolicy(StrictMode.ThreadPolicy.LAX);
        StrictMode.setVmPolicy(StrictMode.VmPolicy.LAX);
    }

    /** The main thread's policy once it is LAX. */
    private static void lax() {
        Log.i(TAG, "lax: thread " + StrictMode.getThreadPolicy());
    }
}
