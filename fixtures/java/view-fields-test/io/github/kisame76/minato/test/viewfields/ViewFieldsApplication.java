package io.github.kisame76.minato.test.viewfields;

import android.app.Activity;
import android.app.Application;
import android.util.Log;
import android.widget.FrameLayout;

import java.lang.reflect.Field;

/**
 * Reflection over framework classes, as engines and libraries use it (the Unity test APK probe: Unity's ReflectionHelper walks a
 * view's superclasses with getDeclaredFields, and the type of every field must resolve): the declared fields of the
 * classes in the chain of a FrameLayout subclass and of other classes engines look at, each readable or the error it
 * gives, and the lookup of a static field along the chain.
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

    private static void log(String line) {
        Log.i(TAG, line);
    }

    /**
     * Only shown: classes whose fields still name a type that framework.jar lacks (TextView's text classifier,
     * Activity's action bar and search manager, Dialog's action bar); the gap list of the batches takes them in.
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
        for (String name : new String[] {"android.widget.TextView", "android.app.Activity", "android.app.Dialog"}) {
            logDevice(readable(name));
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
