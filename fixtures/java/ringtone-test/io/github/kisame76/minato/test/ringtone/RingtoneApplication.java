package io.github.kisame76.minato.test.ringtone;

import android.app.Application;
import android.content.ContentResolver;
import android.content.Context;
import android.content.res.AssetFileDescriptor;
import android.media.AudioAttributes;
import android.media.MediaPlayer;
import android.media.Ringtone;
import android.media.RingtoneManager;
import android.net.Uri;
import android.provider.Settings;
import android.util.Log;

import java.util.ArrayList;
import java.util.List;

/**
 * The RingtoneManager step's test (ADR-0060 e2e1), on a device without sounds set: on Android, its reference runs
 * with the settings' ringtone, notification sound and alarm deleted and their cached files moved away. On a thread
 * that Application.attachBaseContext starts, it logs with the tag ringtone-test:
 * - RingtoneManager's default URIs and types, and the actual defaults;
 * - the settings provider's files: a cached sound and a default sound's URI, read and written;
 * - MediaPlayer.setDataSource with each default sound's URI;
 * - the Ringtone of each default sound: its title, stream, attributes, play, isPlaying and stop; and of no URI, whose
 *   play is the fallback ringtone's (an Ogg Vorbis file Minato does not decode yet), only its title and attributes.
 * It ends with "done".
 */
public class RingtoneApplication extends Application {
    private static final String TAG = "ringtone-test";
    private static final int[] TYPES = {RingtoneManager.TYPE_RINGTONE, RingtoneManager.TYPE_NOTIFICATION,
            RingtoneManager.TYPE_ALARM, RingtoneManager.TYPE_ALL, 0};

    private final List<String> lines = new ArrayList<>();

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        new Thread(() -> {
            try {
                test(base);
            } catch (Throwable t) {
                add("failed: " + t);
            }
            add("done");
            flush();
        }, TAG).start();
    }

    private void add(String line) {
        lines.add(line);
    }

    /** The lines, a pause after every 10 (liblog drops lines). */
    private void flush() {
        int count = 0;
        for (String line : lines) {
            Log.i(TAG, line);
            if (++count % 10 == 0) pause(5);
        }
        lines.clear();
    }

    private static void pause(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    private static String thrown(Throwable t) {
        return t.getClass().getName() + ": " + t.getMessage();
    }

    private void test(Context context) {
        for (int type : TYPES) {
            add("type " + type + ": default " + RingtoneManager.getDefaultUri(type) + ", actual "
                    + RingtoneManager.getActualDefaultRingtoneUri(context, type));
        }
        Uri[] defaults = {Settings.System.DEFAULT_RINGTONE_URI, Settings.System.DEFAULT_NOTIFICATION_URI,
                Settings.System.DEFAULT_ALARM_ALERT_URI};
        Uri[] others = {Uri.parse("content://settings/system/ringtone_cache"),
                Uri.parse("content://settings/system/font_scale"), Uri.parse("content://media/internal/audio/media/1"),
                Uri.parse("file:///nothing.ogg")};
        for (Uri uri : defaults) add(uri + ": type " + RingtoneManager.getDefaultType(uri) + ", default "
                + RingtoneManager.isDefault(uri));
        for (Uri uri : others) add(uri + ": type " + RingtoneManager.getDefaultType(uri) + ", default "
                + RingtoneManager.isDefault(uri));

        // The settings provider's files: the cached sounds and a default's own URI.
        ContentResolver resolver = context.getContentResolver();
        Uri[] files = {Uri.parse("content://settings/system/ringtone_cache"),
                Uri.parse("content://settings/system/notification_sound_cache"),
                Uri.parse("content://settings/system/alarm_alert_cache"), Settings.System.DEFAULT_RINGTONE_URI};
        for (Uri uri : files) {
            for (String mode : new String[] {"r", "w"}) {
                try (AssetFileDescriptor afd = resolver.openAssetFileDescriptor(uri, mode)) {
                    add("open " + uri + " " + mode + ": " + (afd == null ? "null" : "a file"));
                } catch (Exception e) {
                    add("open " + uri + " " + mode + ": " + thrown(e));
                }
            }
        }

        for (Uri uri : defaults) {
            MediaPlayer player = new MediaPlayer();
            try {
                player.setDataSource(context, uri);
                add("player " + uri + ": set");
            } catch (Exception e) {
                add("player " + uri + ": " + thrown(e));
            }
            player.release();
        }

        Uri[] ringtones = {Settings.System.DEFAULT_RINGTONE_URI, Settings.System.DEFAULT_NOTIFICATION_URI,
                Settings.System.DEFAULT_ALARM_ALERT_URI, null};
        for (Uri uri : ringtones) {
            Ringtone ringtone;
            try {
                ringtone = RingtoneManager.getRingtone(context, uri);
            } catch (Exception e) {
                add("ringtone " + uri + ": " + thrown(e));
                continue;
            }
            if (ringtone == null) {
                add("ringtone " + uri + ": null");
                continue;
            }
            AudioAttributes attributes = ringtone.getAudioAttributes();
            add("ringtone " + uri + ": title " + ringtone.getTitle(context) + ", stream " + ringtone.getStreamType()
                    + ", usage " + attributes.getUsage() + ", content " + attributes.getContentType()
                    + ", looping " + ringtone.isLooping() + ", volume " + ringtone.getVolume());
            if (uri == null) continue;
            ringtone.play();
            pause(500);
            add("ringtone " + uri + ": playing " + ringtone.isPlaying());
            ringtone.stop();
            add("ringtone " + uri + ": stopped, playing " + ringtone.isPlaying());
        }
    }
}
