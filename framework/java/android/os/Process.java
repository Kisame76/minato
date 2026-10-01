/*
 * Interim (ADR-0039 step a): the getters of Android 15's android.os.Process that the app process needs before the
 * framework's own build, with their signatures from the tag's class; is64Bit for b3.1's Build. AOSP's Process replaces
 * it when framework.jar is built from the tag's sources (step b3/c): its sources pull in most of the framework, 80,799
 * javac errors on their own.
 */
package android.os;

import android.system.Os;

import dalvik.system.VMRuntime;

public class Process {
    /** The app's process ID, as getpid() answers. */
    public static final int myPid() {
        return Os.getpid();
    }

    /** The calling thread's ID, as gettid() answers. */
    public static final int myTid() {
        return Os.gettid();
    }

    /** The app's uid, as getuid() answers (ADR-0039 decision 3). */
    public static final int myUid() {
        return Os.getuid();
    }

    /** Whether the VM runs 64-bit code, as the tag's Process asks VMRuntime. */
    public static final boolean is64Bit() {
        return VMRuntime.getRuntime().is64Bit();
    }
}
