package io.github.kisame76.minato.test.viewfields;

import android.app.Activity;
import android.app.Application;
import android.util.Log;
import android.widget.FrameLayout;

import java.lang.reflect.Field;

/**
 * Reflection over framework classes, as engines and libraries use it (the Unity test APK probe: Unity's ReflectionHelper walks a
 * view's superclasses with getDeclaredFields, and the type of every field must resolve; retest 10: it looks up an
 * activity's methods, and the Play Games plugin's fragment's, whose parameter and return types must resolve too): the
 * declared fields of the classes in the chain of a FrameLayout subclass and of other classes engines look at, each
 * readable or the error it gives, the lookup of a static field along the chain, and the declared and public methods
 * of the classes in the chain of an Activity subclass and of Fragment, with a method found through getMethods.
 */
public class ViewFieldsApplication extends Application {
    static final String TAG = "view-fields-test";

    /** What Unity's UnityPlayer looks like to the reflection: a FrameLayout with a static Activity field. */
    public static class Player extends FrameLayout {
        public static Activity currentActivity;

        public Player(android.content.Context context) {
            super(context);
        }
    }

    /** What Unity's UnityPlayerActivity looks like to the reflection: an Activity subclass. */
    public static class PlayerActivity extends Activity {
    }

    private static void log(String line) {
        Log.i(TAG, line);
    }

    /**
     * Only shown: TextView, whose fields still name a type that framework.jar lacks (its text classifier), and Dialog,
     * which batch B3b's other part checks; the gap list of the batches takes them in.
     */
    private static void logDevice(String line) {
        Log.i(TAG + "-device", line);
    }

    private static String readable(String name) {
        try {
            Class<?> c = Class.forName(name);
            return c.getName() + ": " + (c.getDeclaredFields().length > 0 ? "fields readable" : "no fields");
        } catch (Throwable t) {
            return name + ": " + t.getClass().getName();
        }
    }

    private static String methods(Class<?> c) {
        String declared;
        try {
            java.lang.reflect.Method[] all = c.getDeclaredMethods();
            for (java.lang.reflect.Method m : all) {
                m.getParameterTypes();
                m.getReturnType();
            }
            declared = all.length > 0 ? "declared methods readable" : "no declared methods";
        } catch (Throwable t) {
            declared = "declared methods " + t.getClass().getName();
        }
        String pub;
        try {
            pub = c.getMethods().length > 0 ? "public methods readable" : "no public methods";
        } catch (Throwable t) {
            pub = "public methods " + t.getClass().getName();
        }
        return c.getName() + ": " + declared + ", " + pub;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        // The chain of the player class.
        for (Class<?> c = Player.class; c != null; c = c.getSuperclass()) {
            if (c == Object.class) {
                break;
            }
            log("chain " + readable(c.getName()));
        }
        String[] classes = {
            "android.view.View", "android.view.ViewGroup", "android.widget.FrameLayout",
            "android.widget.ListView", "android.widget.ScrollView", "android.widget.ImageView",
            "android.view.SurfaceView", "android.view.Surface", "android.view.Window",
            "android.content.Context", "android.content.ContextWrapper", "android.content.res.Resources",
            "android.app.AlertDialog", "android.os.Handler", "android.os.Looper",
        };
        for (String name : classes) {
            log(readable(name));
        }
        for (String name : new String[] {"android.widget.TextView", "android.app.Dialog"}) {
            logDevice(readable(name));
        }
        // The chain of the activity class, and the fragment the Play Games plugin adds.
        for (Class<?> c = PlayerActivity.class; c != null && c != Object.class; c = c.getSuperclass()) {
            log("activity chain " + readable(c.getName()));
            log("activity chain " + methods(c));
        }
        log(readable("android.app.Fragment"));
        log(methods(android.app.Fragment.class));
        try {
            log("activity getClass found " + (PlayerActivity.class.getMethod("getClass") != null));
        } catch (Throwable t) {
            log("activity getClass lookup: " + t.getClass().getName());
        }
        try {
            Field f = null;
            for (Class<?> c = Player.class; f == null && c != null; c = c.getSuperclass()) {
                for (Field candidate : c.getDeclaredFields()) {
                    if (candidate.getName().equals("currentActivity")) {
                        f = candidate;
                        break;
                    }
                }
            }
            log("currentActivity found " + (f != null) + ", type " + (f == null ? "" : f.getType().getName()));
        } catch (Throwable t) {
            log("currentActivity lookup: " + t.getClass().getName());
        }
        log("done");
    }
}
