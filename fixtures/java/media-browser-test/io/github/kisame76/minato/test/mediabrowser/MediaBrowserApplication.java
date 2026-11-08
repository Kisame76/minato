package io.github.kisame76.minato.test.mediabrowser;

import android.app.Application;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.lang.reflect.Field;
import java.lang.reflect.Modifier;
import java.util.Arrays;

/**
 * MediaBrowser against a MediaBrowserService of the app's own, its MediaSession and a MediaController of the session's
 * token: connect, browse, get an item, subscribe, then the controller's reads and transport commands arriving at the
 * session's callback, and the session's state arriving at the controller's callback; each logged.
 */
public class MediaBrowserApplication extends Application {
    static final String TAG = "media-browser-test";

    @Override
    public void onCreate() {
        super.onCreate();
        new Handler(Looper.getMainLooper()).post(this::run);
    }

    private static void constants(String type, String... names) {
        final StringBuilder out = new StringBuilder();
        try {
            final Field[] fields = Class.forName(type).getDeclaredFields();
            Arrays.sort(fields, (a, b) -> a.getName().compareTo(b.getName()));
            for (Field field : fields) {
                final int mods = field.getModifiers();
                if (Arrays.asList(names).contains(field.getName()) && Modifier.isPublic(mods)
                        && Modifier.isStatic(mods) && Modifier.isFinal(mods)) {
                    out.append(field.getName()).append('=').append(field.get(null)).append(' ');
                }
            }
        } catch (Throwable e) {
            out.append("throws ").append(e.getClass().getName());
        }
        Log.i(TAG, type.substring(type.lastIndexOf('.') + 1) + " constants: " + out.toString().trim());
    }

    private void run() {
        constants("android.media.browse.MediaBrowser", "EXTRA_PAGE", "EXTRA_PAGE_SIZE");
        constants("android.media.browse.MediaBrowser$MediaItem", "FLAG_BROWSABLE", "FLAG_PLAYABLE");
        constants("android.media.session.MediaSession", "FLAG_HANDLES_MEDIA_BUTTONS",
                "FLAG_HANDLES_TRANSPORT_CONTROLS");
        constants("android.service.media.MediaBrowserService", "SERVICE_INTERFACE");
        try {
            new BrowserClient(this).start();
        } catch (Throwable e) {
            Log.i(TAG, "MediaBrowser: throws " + e.getClass().getName());
            Log.i(TAG, "done");
        }
    }
}
