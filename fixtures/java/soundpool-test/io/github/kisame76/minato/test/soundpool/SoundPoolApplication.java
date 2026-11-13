package io.github.kisame76.minato.test.soundpool;

import android.app.Application;
import android.media.AudioAttributes;
import android.media.SoundPool;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Map;
import java.util.TreeMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * SoundPool: a pool of two streams on a thread with a Looper, which loads three
 * files the app writes (a short and a long 16-bit PCM WAV and a file of text that no decoder takes), then plays,
 * changes, pauses, stops and unloads. The lines compared are the values SoundPool's contract gives: the sound IDs
 * load returns, the status each onLoadComplete carries, whether play returns a stream ID and which, what unload
 * answers and what happens when a third stream wants a place in a pool of two. Timings go to
 * soundpool-test-device.
 */
public class SoundPoolApplication extends Application {
    private static final String TAG = "soundpool-test";
    private static final int RATE = 8000;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private static void sleep(long millis) {
        try {
            Thread.sleep(millis);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    /** A mono 16-bit PCM WAV of a 440 Hz tone, {@code millis} long. */
    private static void writeWav(File file, int millis) throws IOException {
        final int frames = RATE * millis / 1000;
        final ByteBuffer buffer = ByteBuffer.allocate(44 + frames * 2).order(ByteOrder.LITTLE_ENDIAN);
        buffer.put("RIFF".getBytes()).putInt(36 + frames * 2).put("WAVE".getBytes());
        buffer.put("fmt ".getBytes()).putInt(16).putShort((short) 1).putShort((short) 1).putInt(RATE)
                .putInt(RATE * 2).putShort((short) 2).putShort((short) 16);
        buffer.put("data".getBytes()).putInt(frames * 2);
        for (int i = 0; i < frames; i++) {
            buffer.putShort((short) (6000 * Math.sin(2 * Math.PI * 440 * i / RATE)));
        }
        try (FileOutputStream out = new FileOutputStream(file)) {
            out.write(buffer.array());
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        final HandlerThread events = new HandlerThread("events");
        events.start();
        final Handler handler = new Handler(events.getLooper());
        final Thread driver = new Thread(() -> {
            try {
                run(handler);
            } catch (Throwable thrown) {
                log("failed: " + thrown);
            }
            log("done");
        }, "driver");
        driver.start();
    }

    private void run(Handler handler) throws Exception {
        final File shortFile = new File(getFilesDir(), "short.wav");
        final File longFile = new File(getFilesDir(), "long.wav");
        final File garbage = new File(getFilesDir(), "garbage.wav");
        writeWav(shortFile, 250);
        writeWav(longFile, 3000);
        try (FileOutputStream out = new FileOutputStream(garbage)) {
            out.write("this is not audio".getBytes());
        }

        final Map<Integer, Integer> statuses = new TreeMap<>();
        final CountDownLatch loaded = new CountDownLatch(3);
        final SoundPool[] holder = new SoundPool[1];
        final CountDownLatch made = new CountDownLatch(1);
        // The pool is made on the events thread, so that onLoadComplete arrives there and the driver may wait.
        handler.post(() -> {
            holder[0] = new SoundPool.Builder().setMaxStreams(2)
                    .setAudioAttributes(new AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_GAME)
                            .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build())
                    .build();
            holder[0].setOnLoadCompleteListener((pool, sampleId, status) -> {
                synchronized (statuses) {
                    statuses.put(sampleId, status);
                }
                loaded.countDown();
            });
            made.countDown();
        });
        made.await();
        final SoundPool pool = holder[0];

        final int shortId = pool.load(shortFile.getPath(), 1);
        final int longId = pool.load(longFile.getPath(), 1);
        final int garbageId = pool.load(garbage.getPath(), 1);
        log("load returned " + shortId + ", " + longId + ", " + garbageId);
        log("all three loads completed: " + loaded.await(20, TimeUnit.SECONDS));
        synchronized (statuses) {
            log("short sound status 0: " + (statuses.get(shortId) != null && statuses.get(shortId) == 0));
            log("long sound status 0: " + (statuses.get(longId) != null && statuses.get(longId) == 0));
            log("garbage sound status not 0: " + (statuses.get(garbageId) != null && statuses.get(garbageId) != 0));
        }

        final int garbagePlay = pool.play(garbageId, 1f, 1f, 1, 0, 1f);
        log("play of the sound that did not load: " + garbagePlay);
        log("play of an ID that was never made: " + pool.play(999, 1f, 1f, 1, 0, 1f));

        final int first = pool.play(longId, 1f, 1f, 1, -1, 1f);
        log("play of the long sound, looping: stream " + first);
        sleep(300);
        pool.setRate(first, 2f);
        pool.setVolume(first, 0.5f, 0.25f);
        pool.setPriority(first, 5);
        pool.setLoop(first, 0);
        log("changed rate, volume, priority and loop");
        pool.pause(first);
        sleep(100);
        pool.resume(first);
        log("paused and resumed");
        pool.autoPause();
        sleep(100);
        pool.autoResume();
        log("autoPause and autoResume");

        final int second = pool.play(longId, 1f, 1f, 1, 0, 1f);
        log("second stream: " + second);
        final int third = pool.play(shortId, 1f, 1f, 0, 0, 1f);
        log("third stream with the lowest priority in a pool of two: " + third);
        final int fourth = pool.play(shortId, 1f, 1f, 9, 0, 1f);
        log("fourth stream with the highest priority: stream " + fourth);

        pool.stop(second);
        pool.stop(first);
        pool.stop(fourth);
        pool.stop(12345);
        log("stopped");
        final int again = pool.play(shortId, 1f, 1f, 1, 0, 1f);
        log("play after the stops: stream " + again);

        log("unload of the long sound: " + pool.unload(longId));
        log("unload of it again: " + pool.unload(longId));
        log("unload of an ID that was never made: " + pool.unload(999));
        log("play of the unloaded sound: " + pool.play(longId, 1f, 1f, 1, 0, 1f));
        pool.release();
        log("released");
        handler.getLooper().quitSafely();
    }
}
