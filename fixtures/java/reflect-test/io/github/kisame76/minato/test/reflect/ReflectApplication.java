package io.github.kisame76.minato.test.reflect;

import android.app.Application;
import android.util.Log;
import java.lang.reflect.Method;

/**
 * The test of reflection over the framework's classes, without a window (retest 8's batch B3): for each class that
 * libraries reflect over (dependency injection, serialization, test and mocking libraries, ad SDKs' obfuscators), ART
 * resolves the signature of every declared member when getDeclaredMethods, getDeclaredFields or getDeclaredConstructors
 * is called, and a member that names a class the platform lacks makes the call throw NoClassDefFoundError. It logs under
 * the tag reflect-test whether the calls succeed and whether a public member of the class's API is among the
 * methods. tools/android-reference.sh takes the log from Android 15, and tools/reflect-check.sh compares Minato's with
 * it.
 */
public class ReflectApplication extends Application {
    private static final String TAG = "reflect-test";

    /** A class, and a public method of its API that its declared methods must hold. */
    private static final String[][] CLASSES = {
        {"android.content.Context", "getSystemService"},
        {"android.content.ContextWrapper", "getBaseContext"},
        {"android.app.PictureInPictureParams", "isAutoEnterEnabled"},
        {"android.graphics.ImageDecoder", "setTargetSize"},
        {"android.graphics.drawable.Icon", "loadDrawable"},
        {"android.hardware.Sensor", "getName"},
        {"android.media.MediaDrm", "getPropertyString"},
        {"android.view.textclassifier.TextClassification", "getText"},
        {"android.os.BaseBundle", "getString"},
        {"android.os.UserManager", "isSystemUser"},
        {"android.os.storage.StorageVolume", "getDescription"},
        {"android.view.accessibility.AccessibilityManager", "isEnabled"},
        {"android.hardware.HardwareBuffer", "getWidth"},
        {"android.view.inputmethod.InputContentInfo", "getContentUri"},
        {"android.graphics.HardwareRenderer", "setName"},
        {"android.graphics.RecordingCanvas", "enableZ"},
    };

    @Override
    public void onCreate() {
        super.onCreate();
        for (String[] each : CLASSES) {
            Log.i(TAG, each[0] + ": " + probe(each[0], each[1]));
        }
        Log.i(TAG, "done");
    }

    private static String probe(String name, String method) {
        try {
            Class<?> c = Class.forName(name, false, null);
            boolean found = false;
            for (Method each : c.getDeclaredMethods()) {
                found |= each.getName().equals(method);
            }
            // The constructors and fields are read too, for the signatures they resolve, but their number is not logged:
            // Android's reflection hides the non-SDK constructors and fields of some classes (a Sensor's, an Icon's, a
            // RecordingCanvas's), which no framework class of ours imitates.
            c.getDeclaredConstructors();
            c.getDeclaredFields();
            return "methods " + (c.getDeclaredMethods().length > 0) + ", has " + method + " " + found;
        } catch (Throwable t) {
            return "failed: " + t.getClass().getName();
        }
    }
}
