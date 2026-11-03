package io.github.kisame76.minato.test.soundpoolcodec;

import android.app.Application;
import android.media.SoundPool;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * SoundPool with the compressed files an app ships (the SoundPool plan's
 * steps 3 and 4): each asset named in FILES is copied to the app's files folder, loaded, and played once, and the log
 * says whether its load completed with status 0 and which stream ID its play returned. Android's codecs decode them
 * all; what is heard is not compared.
 */
public class SoundPoolCodecApplication extends Application {
    private static final String TAG = "soundpool-codec-test";
    /**
     * One asset per codec; a step that adds a codec adds its file here and to assets/ (tools/make-tone-samples.sh
     * makes those of step 3: MP3 in stereo and in mono at 22050 Hz, AAC in MP4 and in ADTS, FLAC, ALAC and AC-3 in
     * MP4, and AIFF).
     */
    private static final String[] FILES = {"tone.ogg", "tone.mp3", "tone-mono.mp3", "tone.m4a", "tone.aac",
            "tone.flac", "tone-alac.m4a", "tone-ac3.mp4", "tone.aiff"};

    private static void log(String line) {
        Log.i(TAG, line);
    }

    @Override
    public void onCreate() {
        super.onCreate();
        final HandlerThread events = new HandlerThread("events");
        events.start();
        final Handler handler = new Handler(events.getLooper());
        new Thread(() -> {
            try {
                run(handler);
            } catch (Throwable thrown) {
                log("failed: " + thrown);
            }
            log("done");
            events.quitSafely();
        }, "driver").start();
    }

    private void run(Handler handler) throws Exception {
        final SoundPool[] holder = new SoundPool[1];
        final CountDownLatch made = new CountDownLatch(1);
        final int[] status = new int[1];
        final CountDownLatch[] loaded = new CountDownLatch[1];
        handler.post(() -> {
            holder[0] = new SoundPool.Builder().setMaxStreams(4).build();
            holder[0].setOnLoadCompleteListener((pool, sampleId, result) -> {
                status[0] = result;
                loaded[0].countDown();
            });
            made.countDown();
        });
        made.await();
        final SoundPool pool = holder[0];
        for (String name : FILES) {
            final File file = new File(getFilesDir(), name);
            try (InputStream in = getAssets().open(name); FileOutputStream out = new FileOutputStream(file)) {
                final byte[] chunk = new byte[4096];
                for (int n; (n = in.read(chunk)) > 0; ) {
                    out.write(chunk, 0, n);
                }
            }
            loaded[0] = new CountDownLatch(1);
            status[0] = -99;
            final int id = pool.load(file.getPath(), 1);
            log(name + ": load returned " + id);
            log(name + ": load completed: " + loaded[0].await(20, TimeUnit.SECONDS));
            log(name + ": status 0: " + (status[0] == 0));
            log(name + ": play: stream " + pool.play(id, 1f, 1f, 1, 0, 1f));
        }
        pool.release();
        log("released");
    }
}
