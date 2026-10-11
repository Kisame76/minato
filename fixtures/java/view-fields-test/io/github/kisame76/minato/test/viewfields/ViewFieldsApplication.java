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
 * of the classes in the chain of an Activity subclass and of Fragment, with a method found through getMethods; and
 * (retest 11: the game's device id walks the content resolver's class) the same for the classes behind a context's
 * own objects and about 60 classes engines and SDKs call; and (retest 12) the storage manager, WebView and the locale
 * manager, whose answers Helpshift reads.
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
     * Only shown: TextView and Dialog, which batch B3b's other part checks, and the classes whose members still name
     * types framework.jar lacks; the gap list of the batches takes them in.
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

    /** A class's fields, declared methods and public methods, each readable or the error they give. */
    private static String verdict(Class<?> c) {
        String fields;
        try {
            c.getDeclaredFields();
            fields = "fields readable";
        } catch (Throwable t) {
            fields = "fields " + t.getClass().getName();
        }
        String all = methods(c);
        return fields + ", " + all.substring(all.indexOf(": ") + 2);
    }

    private Class<?> objectClass(String what) throws Exception {
        switch (what) {
            case "content resolver": return getContentResolver().getClass();
            case "package manager": return getPackageManager().getClass();
            case "base context": return getBaseContext().getClass();
            case "window manager": return getSystemService(android.view.WindowManager.class).getClass();
            case "shared preferences": return getSharedPreferences("view-fields", 0).getClass();
            case "preferences editor": return getSharedPreferences("view-fields", 0).edit().getClass();
            case "display metrics": return getResources().getDisplayMetrics().getClass();
            default: return getAssets().getClass();
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
        // The classes behind a context's own objects, which engines call methods on (retest 11: the content
        // resolver), by what they are rather than by their class names, and the classes engines and SDKs look at.
        String[][] objects = {
            {"content resolver", ""}, {"package manager", ""}, {"base context", ""}, {"window manager", ""},
            {"shared preferences", ""}, {"preferences editor", ""}, {"display metrics", ""}, {"assets", ""},
        };
        for (String[] object : objects) {
            try {
                Class<?> c = objectClass(object[0]);
                log(object[0] + " " + verdict(c));
            } catch (Throwable t) {
                log(object[0] + ": " + t.getClass().getName());
            }
        }
        String[] names = {
            "android.content.ContentResolver", "android.content.pm.PackageManager", "android.content.res.Resources",
            "android.content.res.AssetManager", "android.view.Window", "com.android.internal.policy.PhoneWindow",
            "android.view.WindowManager", "android.view.WindowManagerImpl", "android.view.Display", "android.os.Looper",
            "android.os.Handler", "android.content.SharedPreferences", "android.content.SharedPreferences$Editor",
            "android.provider.Settings", "android.provider.Settings$Secure", "android.provider.Settings$System",
            "android.provider.Settings$Global", "android.os.Build", "android.os.Build$VERSION", "android.content.Intent",
            "android.os.Bundle", "android.net.Uri", "android.app.Application", "android.app.ContextImpl",
            "android.view.View", "android.view.ViewGroup", "android.widget.FrameLayout", "android.view.SurfaceView",
            "android.view.SurfaceHolder", "android.view.Surface", "android.app.NotificationManager",
            "android.media.AudioManager", "android.net.ConnectivityManager", "android.net.NetworkInfo",
            "android.telephony.TelephonyManager", "android.os.Vibrator", "android.util.DisplayMetrics",
            "android.content.res.Configuration", "android.content.pm.PackageInfo", "android.content.pm.ApplicationInfo",
            "android.content.pm.ActivityInfo", "android.os.Environment", "android.os.Process",
            "android.os.SystemClock", "android.view.Choreographer", "android.view.inputmethod.InputMethodManager",
            "android.view.MotionEvent", "android.view.KeyEvent", "android.app.AlertDialog", "android.app.Dialog",
            "android.widget.TextView", "android.widget.EditText", "android.os.PowerManager",
            "android.app.ActivityManager", "android.app.PendingIntent", "android.content.ClipboardManager",
            "android.hardware.SensorManager", "android.os.Debug", "android.os.StatFs", "android.view.Display$Mode",
            "android.view.WindowInsets", "android.content.res.TypedArray", "android.graphics.Bitmap",
            "android.graphics.Canvas",
        };
        for (String name : names) {
            try {
                log(name + " " + verdict(Class.forName(name)));
            } catch (Throwable t) {
                log(name + ": " + t.getClass().getName());
            }
        }
        // Retest 12's resolve list: the storage manager (its OBB listener), WebView's members, and Helpshift's
        // language through the locale manager.
        for (String name : new String[] {"android.os.storage.StorageManager", "android.webkit.WebView",
                "android.app.LocaleManager"}) {
            try {
                log(name + " " + verdict(Class.forName(name)));
            } catch (Throwable t) {
                log(name + ": " + t.getClass().getName());
            }
        }
        try {
            android.app.LocaleManager locales = getSystemService(android.app.LocaleManager.class);
            log("locale manager " + (locales != null));
            log("application locales \"" + locales.getApplicationLocales().toLanguageTags() + "\"");
            log("system locales same as the configuration's "
                    + locales.getSystemLocales().equals(getResources().getConfiguration().getLocales()));
        } catch (Throwable t) {
            log("locale manager: " + t.getClass().getName());
        }
        log("done");
    }
}
