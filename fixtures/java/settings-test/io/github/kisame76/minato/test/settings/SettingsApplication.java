package io.github.kisame76.minato.test.settings;

import android.app.Application;
import android.content.ContentResolver;
import android.database.ContentObserver;
import android.database.Cursor;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.speech.tts.TextToSpeech;
import android.speech.tts.UtteranceProgressListener;
import android.util.Log;
import android.view.ViewConfiguration;

import java.util.Arrays;

/**
 * ADR-0045's test of the settings that Godot's start reads, in Application.onCreate, before its
 * launcher activity starts:
 * <ul>
 *   <li>the app's ContentResolver;
 *   <li>the four keys behind ViewConfiguration's touch and key-repeat times, each read from Settings.Secure with a
 *       sentinel, so that the line says whether the device stores it or refuses it, beside ViewConfiguration's getter;
 *   <li>values apps read: System's screen-off timeout and auto-rotate, Global's airplane mode, and Secure's android_id
 *       by its form only (16 hex digits), which is per signing key, its value as a device line;
 *   <li>a key no table has, a key that is readable only up to a target SDK below the app's, and a key moved from
 *       System to Global;
 *   <li>a write to each table, which the app may not make;
 *   <li>queries of the provider: a key by its URI, with the columns it answers, and a hidden key by name;
 *   <li>a content observer on a setting;
 *   <li>text to speech without an engine (13b), with the calls Godot's speech class makes: the AVD has none, so
 *       onInit answers ERROR, already in the constructor, and what needs an engine answers its error.
 * </ul>
 * apk-runner-35 is wiped first, so its values are its image's. An exception's message that names the app's uid is
 * left out.
 */
public class SettingsApplication extends Application {
    private static final String TAG = "settings-test";
    /** Lines that differ by device, which the check only shows. */
    private static final String DEVICE_TAG = "settings-test-device";

    private interface Section {
        void run() throws Exception;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        ContentResolver resolver = getContentResolver();
        guarded("content resolver", () -> Log.i(TAG, "content resolver: " + resolver.getClass().getName()));
        guarded("view configuration", () -> viewConfiguration(resolver));
        guarded("values", () -> values(resolver));
        guarded("refused reads", () -> refusedReads(resolver));
        guarded("writes", () -> writes(resolver));
        guarded("queries", () -> queries(resolver));
        guarded("content observer", () -> observer(resolver));
        guarded("text to speech", this::textToSpeech);
        Log.i(TAG, "done");
    }

    private void viewConfiguration(ContentResolver resolver) {
        ViewConfiguration.get(this);
        // The keys' names, which are hidden API.
        Log.i(TAG, "long press: secure " + secureInt(resolver, "long_press_timeout")
                + ", ViewConfiguration " + ViewConfiguration.getLongPressTimeout());
        Log.i(TAG, "multi press: secure " + secureInt(resolver, "multi_press_timeout")
                + ", ViewConfiguration " + ViewConfiguration.getMultiPressTimeout());
        Log.i(TAG, "key repeat timeout: secure " + secureInt(resolver, "key_repeat_timeout")
                + ", ViewConfiguration " + ViewConfiguration.getKeyRepeatTimeout());
        Log.i(TAG, "key repeat delay: secure " + secureInt(resolver, "key_repeat_delay")
                + ", ViewConfiguration " + ViewConfiguration.getKeyRepeatDelay());
    }

    private static String secureInt(ContentResolver resolver, String name) {
        try {
            return Integer.toString(Settings.Secure.getInt(resolver, name, -1));
        } catch (SecurityException e) {
            return e.toString();
        }
    }

    private void values(ContentResolver resolver) throws Exception {
        Log.i(TAG, "system: screen off timeout "
                + Settings.System.getInt(resolver, Settings.System.SCREEN_OFF_TIMEOUT) + ", auto-rotate "
                + Settings.System.getInt(resolver, Settings.System.ACCELEROMETER_ROTATION));
        Log.i(TAG, "global: airplane mode " + Settings.Global.getInt(resolver, Settings.Global.AIRPLANE_MODE_ON));
        String id = Settings.Secure.getString(resolver, Settings.Secure.ANDROID_ID);
        Log.i(TAG, "secure: android_id 16 hex digits " + (id != null && id.matches("[0-9a-f]{16}"))
                + ", the same again " + (id != null && id.equals(Settings.Secure.getString(resolver,
                Settings.Secure.ANDROID_ID))));
        Log.i(DEVICE_TAG, "android_id " + id);
    }

    private void refusedReads(ContentResolver resolver) {
        try {
            Log.i(TAG, "a key no table has: " + Settings.Global.getInt(resolver, "minato_no_such_setting"));
        } catch (Settings.SettingNotFoundException e) {
            Log.i(TAG, "a key no table has: " + e + ", as a string " + Settings.Global.getString(resolver,
                    "minato_no_such_setting"));
        }
        try {
            Log.i(TAG, "bluetooth address: " + Settings.Secure.getString(resolver, "bluetooth_address"));
        } catch (SecurityException e) {
            Log.i(TAG, "bluetooth address: " + e);
        }
        Log.i(TAG, "airplane mode through System: " + Settings.System.getString(resolver, "airplane_mode_on"));
    }

    private void writes(ContentResolver resolver) {
        try {
            Log.i(TAG, "system write: " + Settings.System.putInt(resolver, Settings.System.SCREEN_BRIGHTNESS, 10));
        } catch (RuntimeException e) {
            Log.i(TAG, "system write: " + e.getClass().getName());
        }
        try {
            Log.i(TAG, "secure write: " + Settings.Secure.putInt(resolver, "minato_test", 1));
        } catch (RuntimeException e) {
            Log.i(TAG, "secure write: " + e.getClass().getName());
        }
        try {
            Log.i(TAG, "global write: " + Settings.Global.putInt(resolver, "minato_test", 1));
        } catch (RuntimeException e) {
            Log.i(TAG, "global write: " + e.getClass().getName());
        }
    }

    private void queries(ContentResolver resolver) {
        try (Cursor cursor = resolver.query(Settings.System.getUriFor(Settings.System.SCREEN_OFF_TIMEOUT), null, null,
                null, null)) {
            Log.i(TAG, "query: " + cursor.getCount() + " row, columns " + Arrays.toString(cursor.getColumnNames())
                    + ", screen off timeout " + (cursor.moveToFirst() ? cursor.getString(
                    cursor.getColumnIndexOrThrow(Settings.NameValueTable.VALUE)) : "none"));
        }
        try (Cursor cursor = resolver.query(Settings.Secure.CONTENT_URI, new String[] {Settings.NameValueTable.VALUE},
                "name=?", new String[] {"key_repeat_timeout"}, null)) {
            Log.i(TAG, "query of a hidden key: " + cursor.getCount() + " rows");
        } catch (SecurityException e) {
            Log.i(TAG, "query of a hidden key: " + e);
        }
    }

    private void observer(ContentResolver resolver) {
        ContentObserver observer = new ContentObserver(new Handler(Looper.getMainLooper())) {
            @Override
            public void onChange(boolean selfChange) {
                Log.i(TAG, "content observer: changed");
            }
        };
        resolver.registerContentObserver(Settings.System.getUriFor(Settings.System.ACCELEROMETER_ROTATION), false,
                observer);
        resolver.unregisterContentObserver(observer);
        Log.i(TAG, "content observer: registered and unregistered");
    }

    private void textToSpeech() {
        int[] init = {Integer.MIN_VALUE};
        TextToSpeech tts = new TextToSpeech(this, status -> init[0] = status);
        Log.i(TAG, "tts: onInit " + (init[0] == Integer.MIN_VALUE ? "not called" : init[0])
                + " when the constructor returned");
        Log.i(TAG, "tts: engines " + tts.getEngines().size() + ", default engine " + tts.getDefaultEngine());
        int listener = tts.setOnUtteranceProgressListener(new UtteranceProgressListener() {
            @Override
            public void onStart(String utteranceId) {
                Log.i(TAG, "tts: started " + utteranceId);
            }

            @Override
            public void onDone(String utteranceId) {
                Log.i(TAG, "tts: done " + utteranceId);
            }

            @Override
            public void onError(String utteranceId) {
                Log.i(TAG, "tts: error " + utteranceId);
            }
        });
        Log.i(TAG, "tts: listener " + listener + ", rate " + tts.setSpeechRate(1.5f) + ", pitch "
                + tts.setPitch(0.8f));
        Log.i(TAG, "tts: speak " + tts.speak("Minato", TextToSpeech.QUEUE_FLUSH, null, "settings-test")
                + ", speaking " + tts.isSpeaking() + ", stop " + tts.stop() + ", voices " + tts.getVoices());
        tts.shutdown();
        Log.i(TAG, "tts: shut down");
    }

    /** Runs a section; what it throws is logged with its name. */
    private static void guarded(String name, Section section) {
        try {
            section.run();
        } catch (Throwable e) {
            Log.i(TAG, name + ": " + e);
        }
    }
}
