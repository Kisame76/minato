package io.github.kisame76.minato.test.resolvelog;

import android.app.Application;
import android.util.Pair;

/**
 * The log of failed resolutions (minato-resolve, the Unity test APK probe: Unity's activity died on a missing
 * android.app.Dialog whose NoClassDefFoundError it caught, so nothing named the gap): ART logs each class, method and
 * field it fails to resolve once, with the method that asks, even when the app catches the error. The app was compiled
 * against compile-only/, which D8 leaves out: the class Absent and a twin of android.util.Pair with a method and two
 * fields the real one lacks. Each failure happens twice and must be logged once. Each line goes to System.out with the
 * prefix "resolve-log-test: ".
 */
public class ResolveLogApplication extends Application {
    static final String TAG = "resolve-log-test";

    private static void print(String line) {
        System.out.println(TAG + ": " + line);
    }

    static Object newAbsent() {
        return new Absent();
    }

    static int callMissing() {
        return Pair.minatoMissing();
    }

    static int readMissingStatic() {
        return Pair.minatoMissingStatic;
    }

    static int readMissingField() {
        return new Pair<Object, Object>(null, null).minatoMissingField;
    }

    private static String outcome(int which) {
        try {
            switch (which) {
                case 0:
                    newAbsent();
                    break;
                case 1:
                    callMissing();
                    break;
                case 2:
                    readMissingStatic();
                    break;
                default:
                    readMissingField();
                    break;
            }
            return "no error";
        } catch (Throwable e) {
            return e.getClass().getName();
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        for (int round = 1; round <= 2; round++) {
            print("round " + round + ", a missing class: " + outcome(0));
            print("round " + round + ", a missing method: " + outcome(1));
            print("round " + round + ", a missing static field: " + outcome(2));
            print("round " + round + ", a missing instance field: " + outcome(3));
        }
        print("done");
    }
}
