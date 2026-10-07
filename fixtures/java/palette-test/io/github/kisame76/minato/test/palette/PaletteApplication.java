package io.github.kisame76.minato.test.palette;

import android.app.Application;
import android.content.res.Resources;
import android.util.Log;
import android.view.ContextThemeWrapper;
import android.widget.TextView;

import java.lang.reflect.Field;
import java.lang.reflect.Modifier;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;

/**
 * ADR-0043 step f)'s test, Monet's palette. Android 15's SystemUI lays three fabricated overlays over framework-res
 * at run time, accent, neutral and dynamic, made from the wallpaper's seed color, and leaves framework-res's own values
 * as they are. Under the tag palette-test it logs every public system color, the fields of android.R.color named
 * system_* in the order of their names, as the app's Resources give it and as Resources.getSystem() gives it; how many
 * differ between the two; a TextView's default text and hint colors on Theme.DeviceDefault and on
 * Theme.DeviceDefault.Light; and "done". A color that fails logs "failed" and the exception. tools/android-reference.sh
 * takes the log from Android 15 on apk-runner-35, and tools/palette-check.sh compares Minato's with it.
 */
public class PaletteApplication extends Application {
    private static final String TAG = "palette-test";

    @Override
    public void onCreate() {
        super.onCreate();
        Resources app = getResources();
        Resources system = Resources.getSystem();
        List<String> names = new ArrayList<>();
        for (Field field : android.R.color.class.getFields()) {
            if (Modifier.isStatic(field.getModifiers()) && field.getName().startsWith("system_")) {
                names.add(field.getName());
            }
        }
        Collections.sort(names);
        int differ = 0;
        for (String name : names) {
            try {
                int id = android.R.color.class.getField(name).getInt(null);
                int appColor = app.getColor(id, null);
                int systemColor = system.getColor(id, null);
                if (appColor != systemColor) {
                    differ++;
                }
                Log.i(TAG, name + ": app " + hex(appColor) + ", system " + hex(systemColor));
            } catch (Exception e) {
                Log.i(TAG, name + ": failed " + e);
            }
        }
        Log.i(TAG, names.size() + " system colors, " + differ + " differ between the app's and the system's");
        textView("Theme.DeviceDefault", android.R.style.Theme_DeviceDefault);
        textView("Theme.DeviceDefault.Light", android.R.style.Theme_DeviceDefault_Light);
        Log.i(TAG, "done");
    }

    private void textView(String theme, int id) {
        try {
            TextView view = new TextView(new ContextThemeWrapper(this, id));
            Log.i(TAG, "text view on " + theme + ": text " + hex(view.getCurrentTextColor()) + ", hint "
                    + hex(view.getCurrentHintTextColor()));
        } catch (Exception e) {
            Log.i(TAG, "text view on " + theme + ": failed " + e);
        }
    }

    private static String hex(int color) {
        return String.format(Locale.ROOT, "#%08X", color);
    }
}
