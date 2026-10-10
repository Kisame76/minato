package io.github.kisame76.minato.test.jnifields;

import android.app.Activity;
import android.app.Application;
import android.util.Log;

import java.lang.reflect.Field;
import java.lang.reflect.Modifier;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * Fields through java.lang.reflect and JNI (the Unity test APK probe: Unity's GetStatic of UnityPlayer.currentActivity asks
 * reflection for the field, takes its type's signature, and asks JNI's GetStaticFieldID with it; where reflection
 * fails, it asks with the C# type's signature, Ljava/lang/Object;, and JNI refuses). Reflection's view of static and
 * instance fields, public, private and inherited, their types and the types' names; Class.forName with the names a
 * signature turns into; JNI's lookups, exact, with a supertype's signature, of the wrong kind and missing, with the
 * messages of their NoSuchFieldErrors; FromReflectedField and ToReflectedField; and FindClass on the calling thread and
 * on a thread the library attaches.
 */
public class JniFieldsApplication extends Application {
    static final String TAG = "jni-fields-test";
    static final String SELF = "io/github/kisame76/minato/test/jnifields/JniFieldsApplication";

    public static Activity currentActivity;
    public static Object anything = "a string";
    public static int[] numbers = {1, 2};
    public static String[][] names;
    public static long count = 7;
    private static Activity hiddenActivity;
    public Activity ownActivity;
    public final String text = "own";

    static native String staticLookup(Class<?> type, String name, String signature);
    static native String instanceLookup(Class<?> type, String name, String signature);
    static native String fromReflected(Field field);
    static native String toReflected(Class<?> type, String name, String signature, boolean isStatic);
    static native String findClassHere(String name);
    static native String findClassOnAttachedThread(String name);

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private interface Action {
        Object run() throws Exception;
    }

    private static String outcome(Action action) {
        try {
            return String.valueOf(action.run());
        } catch (Throwable e) {
            // ART names the APK's path, which differs by install, after the message.
            String message = String.valueOf(e.getMessage());
            int path = message.indexOf(" (declaration of ");
            return e.getClass().getName() + ": " + (path < 0 ? message : message.substring(0, path));
        }
    }

    private static String describe(Field field) {
        return field.getName() + " " + field.getType().getName() + " of " + field.getDeclaringClass().getSimpleName()
                + (Modifier.isStatic(field.getModifiers()) ? " static" : " instance")
                + (Modifier.isPublic(field.getModifiers()) ? " public" : "");
    }

    @Override
    public void onCreate() {
        super.onCreate();
        System.loadLibrary("jni-fields-test");
        Class<?> self = JniFieldsApplication.class;

        log("getField currentActivity: " + outcome(() -> describe(self.getField("currentActivity"))));
        log("getField ownActivity: " + outcome(() -> describe(self.getField("ownActivity"))));
        log("getField hiddenActivity: " + outcome(() -> describe(self.getField("hiddenActivity"))));
        log("getDeclaredField hiddenActivity: " + outcome(() -> describe(self.getDeclaredField("hiddenActivity"))));
        log("getDeclaredField of a superclass's field: " + outcome(() -> describe(self.getDeclaredField("mBase"))));
        log("getField of a public superclass field: " + outcome(() -> describe(self.getField("TRIM_MEMORY_COMPLETE"))));
        List<String> mine = new ArrayList<>();
        for (Field field : self.getFields()) {
            if (field.getDeclaringClass() == self) mine.add(field.getName());
        }
        Collections.sort(mine);
        log("getFields, this class's: " + mine);
        List<String> declared = new ArrayList<>();
        for (Field field : self.getDeclaredFields()) declared.add(describe(field));
        Collections.sort(declared);
        log("getDeclaredFields: " + declared);
        log("Object assignable from Activity: " + Object.class.isAssignableFrom(Activity.class)
                + ", Activity from Object: " + Activity.class.isAssignableFrom(Object.class)
                + ", currentActivity's type from Activity: "
                + outcome(() -> self.getField("currentActivity").getType().isAssignableFrom(Activity.class)));
        log("type names: " + outcome(() -> self.getField("numbers").getType().getName() + " "
                + self.getField("names").getType().getName() + " " + self.getField("count").getType().getName() + " "
                + self.getField("anything").getType().getName()));
        log("forName android.app.Activity: " + outcome(() -> Class.forName("android.app.Activity").getName()));
        log("forName [Ljava.lang.Object;: " + outcome(() -> Class.forName("[Ljava.lang.Object;").getName()));
        log("forName Landroid/app/Activity;: " + outcome(() -> Class.forName("Landroid/app/Activity;").getName()));
        log("forName in the app's loader: " + outcome(() -> Class.forName(self.getName(), false,
                self.getClassLoader()).getSimpleName()));
        log("field value through reflection: " + outcome(() -> self.getField("anything").get(null)));

        log("JNI static, exact: " + staticLookup(self, "currentActivity", "Landroid/app/Activity;"));
        log("JNI static, Object: " + staticLookup(self, "currentActivity", "Ljava/lang/Object;"));
        log("JNI static, a subclass's signature: " + staticLookup(self, "anything", "Ljava/lang/String;"));
        log("JNI static, private: " + staticLookup(self, "hiddenActivity", "Landroid/app/Activity;"));
        log("JNI static, an instance field: " + staticLookup(self, "ownActivity", "Landroid/app/Activity;"));
        log("JNI static, missing: " + staticLookup(self, "nothing", "I"));
        log("JNI static, a primitive: " + staticLookup(self, "count", "J"));
        log("JNI static, a primitive as int: " + staticLookup(self, "count", "I"));
        log("JNI instance, exact: " + instanceLookup(self, "ownActivity", "Landroid/app/Activity;"));
        log("JNI instance, Object: " + instanceLookup(self, "ownActivity", "Ljava/lang/Object;"));
        log("JNI instance, a static field: " + instanceLookup(self, "currentActivity", "Landroid/app/Activity;"));
        log("JNI instance, a superclass's private field: " + instanceLookup(self, "mBase",
                "Landroid/content/Context;"));
        log("FromReflectedField anything: " + outcome(() -> fromReflected(self.getField("anything"))));
        log("FromReflectedField currentActivity: " + outcome(() -> fromReflected(self.getField("currentActivity"))));
        log("ToReflectedField anything: " + toReflected(self, "anything", "Ljava/lang/Object;", true));
        log("ToReflectedField ownActivity: " + toReflected(self, "ownActivity", "Landroid/app/Activity;", false));
        log("FindClass of this class here: " + findClassHere(SELF));
        log("FindClass of Activity here: " + findClassHere("android/app/Activity"));
        log("FindClass of this class on an attached thread: " + findClassOnAttachedThread(SELF));
        log("FindClass of Activity on an attached thread: " + findClassOnAttachedThread("android/app/Activity"));
        log("done");
    }
}
