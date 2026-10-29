package io.github.kisame76.minato.test.mediaplayer;

import android.app.Application;
import android.content.Context;
import android.content.res.AssetFileDescriptor;
import android.graphics.ImageFormat;
import android.media.Image;
import android.media.ImageReader;
import android.media.MediaDataSource;
import android.media.MediaFormat;
import android.media.MediaPlayer;
import android.media.PlaybackParams;
import android.net.Uri;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.PersistableBundle;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * ADR-0060 e2e's test of android.media.MediaPlayer, on Minato's own baseline.mp4 (an H.264 track of 30 frames and an
 * AAC track, res/raw). On a thread that Application.attachBaseContext starts, whose players' callbacks come on the
 * main thread, it logs with the tag media-player-test:
 * - a new player's answers in the idle state, and their callbacks;
 * - the raw resource's file descriptor prepared: duration, video size, tracks, metrics, the callbacks;
 * - seeks in each mode while paused, and the positions they give;
 * - a playback to its end into an ImageReader's Surface (the frames it got to the device tag), what it answers then,
 *   and a second start;
 * - looping, volume and playback parameters; reset and release, and the answers after each;
 * - the other sources: a path, a file Uri, a FileDescriptor, a MediaDataSource, MediaPlayer.create, a missing path,
 *   bytes that are no movie, and http and https URLs on 127.0.0.1 (refused by the network security config with the
 *   INTERNET permission, and without it in the twin media-player-test-offline);
 * - DRM: the DRM info and prepareDrm for Widevine and ClearKey.
 * It logs with the tag media-player-test (the twin with media-player-test-offline), a line that starts with "device "
 * to that tag with "-device", and ends with "done".
 */
public class MediaPlayerApplication extends Application {
    private String tag = "media-player-test";
    private static final UUID WIDEVINE = new UUID(0xedef8ba979d64aceL, 0xa3c827dcd51d21edL);
    private static final UUID CLEARKEY = new UUID(0xe2719d58a985b3c9L, 0x781ab030af78d30eL);

    private final List<String> lines = new ArrayList<>();

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        if (base.getPackageName().endsWith("offline")) tag = "media-player-test-offline";
        // On a thread of its own, without a Looper: the players' callbacks come on the main thread meanwhile.
        new Thread(() -> {
            try {
                test(base);
            } catch (Throwable t) {
                add("failed: " + t);
                add("device " + Log.getStackTraceString(t));
            }
            add("done");
            flush();
        }, tag).start();
    }

    private void add(String line) {
        lines.add(line);
    }

    /** The lines, those that start with "device " to the device tag, a pause after every 10 (liblog drops lines). */
    private void flush() {
        int count = 0;
        for (String line : lines) {
            for (String part : line.split("\n")) {
                if (line.startsWith("device ")) {
                    Log.i(tag + "-device", part.startsWith("device ") ? part.substring(7) : part);
                } else {
                    Log.i(tag, part);
                }
                if (++count % 10 == 0) pause(5);
            }
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

    /** A player's callbacks, in the order they came. */
    private static final class Events {
        final LinkedBlockingQueue<String> queue = new LinkedBlockingQueue<>();
        final List<String> buffering = Collections.synchronizedList(new ArrayList<>());

        Events(MediaPlayer player) {
            player.setOnPreparedListener(p -> queue.add("prepared"));
            player.setOnCompletionListener(p -> queue.add("completion"));
            player.setOnSeekCompleteListener(p -> queue.add("seek complete"));
            player.setOnVideoSizeChangedListener((p, width, height) -> queue.add("video size " + width + "x" + height));
            player.setOnErrorListener((p, what, extra) -> {
                queue.add("error " + what + " " + extra);
                return true;
            });
            player.setOnInfoListener((p, what, extra) -> {
                queue.add("info " + what + " " + extra);
                return false;
            });
            player.setOnBufferingUpdateListener((p, percent) -> buffering.add("buffering " + percent));
        }

        /** The callbacks up to and with the first that starts with one of `ends`, at most `ms` later. */
        List<String> until(long ms, String... ends) {
            List<String> got = new ArrayList<>();
            long deadline = System.currentTimeMillis() + ms;
            while (true) {
                long left = deadline - System.currentTimeMillis();
                String event = null;
                try {
                    event = left > 0 ? queue.poll(left, TimeUnit.MILLISECONDS) : null;
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                }
                if (event == null) {
                    got.add("(none in " + ms + " ms)");
                    return got;
                }
                got.add(event);
                for (String end : ends) {
                    if (event.startsWith(end)) return got;
                }
            }
        }

        /** The callbacks that come within `ms`. */
        List<String> within(long ms) {
            pause(ms);
            List<String> got = new ArrayList<>();
            queue.drainTo(got);
            return got;
        }
    }

    private interface Call {
        Object call() throws Exception;
    }

    private interface VoidCall {
        void call() throws Exception;
    }

    private static String answer(Call call) {
        try {
            return String.valueOf(call.call());
        } catch (Throwable t) {
            return t.getClass().getName() + (t.getMessage() == null ? "" : ": " + t.getMessage());
        }
    }

    private static String run(VoidCall call) {
        try {
            call.call();
            return "ok";
        } catch (Throwable t) {
            return t.getClass().getName() + (t.getMessage() == null ? "" : ": " + t.getMessage());
        }
    }

    private void test(Context context) throws Exception {
        int raw = context.getResources().getIdentifier("baseline", "raw", context.getPackageName());
        byte[] bytes = read(context, raw);
        File copy = new File(context.getFilesDir(), "baseline.mp4");
        try (FileOutputStream out = new FileOutputStream(copy)) {
            out.write(bytes);
        }

        idle();
        prepared(context, raw);
        sources(context, raw, copy, bytes);
        network();
        drm(context, raw);
    }

    private static byte[] read(Context context, int raw) throws IOException {
        try (InputStream in = context.getResources().openRawResource(raw)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] chunk = new byte[8192];
            for (int count; (count = in.read(chunk)) > 0;) out.write(chunk, 0, count);
            return out.toByteArray();
        }
    }

    /** A new player's answers before a source. */
    private void idle() {
        MediaPlayer player = new MediaPlayer();
        Events events = new Events(player);
        add("idle: position " + answer(player::getCurrentPosition) + ", duration " + answer(player::getDuration)
                + ", playing " + answer(player::isPlaying) + ", size " + answer(player::getVideoWidth) + "x"
                + answer(player::getVideoHeight) + ", looping " + answer(player::isLooping));
        add("idle: start " + run(player::start) + ", pause " + run(player::pause) + ", stop " + run(player::stop));
        add("idle: seek " + run(() -> player.seekTo(0)) + ", prepare " + run(player::prepare) + ", prepareAsync "
                + run(player::prepareAsync));
        add("idle: tracks " + answer(() -> player.getTrackInfo().length) + ", playback params "
                + answer(() -> player.getPlaybackParams().getSpeed()));
        add("idle: callbacks " + events.within(300));
        player.reset();
        add("after reset: start " + run(player::start) + ", position " + answer(player::getCurrentPosition));
        add("after reset: callbacks " + events.within(300));
        player.release();
        add("after release: playing " + answer(player::isPlaying) + ", start " + run(player::start) + ", duration "
                + answer(player::getDuration));
    }

    /** The raw resource's descriptor: prepared, seeks, a playback to its end, the parameters, reset and release. */
    private void prepared(Context context, int raw) throws Exception {
        MediaPlayer player = new MediaPlayer();
        Events events = new Events(player);
        try (AssetFileDescriptor asset = context.getResources().openRawResourceFd(raw)) {
            add("fd: set " + run(() -> player.setDataSource(asset)) + ", set again "
                    + run(() -> player.setDataSource(asset)));
        }
        add("fd: prepare " + run(player::prepare) + ", callbacks " + events.until(5000, "prepared"));
        add("fd: duration " + answer(player::getDuration) + ", size " + player.getVideoWidth() + "x"
                + player.getVideoHeight() + ", position " + answer(player::getCurrentPosition) + ", playing "
                + player.isPlaying() + ", prepare again " + run(player::prepare));
        MediaPlayer.TrackInfo[] tracks = player.getTrackInfo();
        add("fd: " + tracks.length + " tracks");
        for (int index = 0; index < tracks.length; index++) {
            MediaPlayer.TrackInfo track = tracks[index];
            MediaFormat format = track.getFormat();
            add("fd track " + index + ": type " + track.getTrackType() + ", language " + track.getLanguage()
                    + ", format " + (format == null ? "none" : format.getString(MediaFormat.KEY_MIME)));
        }
        add("fd: selected video " + answer(() -> player.getSelectedTrack(MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_VIDEO))
                + ", audio " + answer(() -> player.getSelectedTrack(MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_AUDIO))
                + ", select 0 " + run(() -> player.selectTrack(0)) + ", deselect 1 "
                + run(() -> player.deselectTrack(1)));
        metrics("fd", player.getMetrics());

        int[] modes = {MediaPlayer.SEEK_PREVIOUS_SYNC, MediaPlayer.SEEK_NEXT_SYNC, MediaPlayer.SEEK_CLOSEST_SYNC,
                MediaPlayer.SEEK_CLOSEST};
        for (int mode : modes) {
            for (int time : new int[] {0, 433, 500}) {
                String set = run(() -> player.seekTo(time, mode));
                List<String> callbacks = events.until(5000, "seek complete");
                add("fd: seek " + time + " mode " + mode + " " + set + ", "
                        + (callbacks.contains("seek complete") ? "seek complete" : "no seek complete")
                        + ", position " + player.getCurrentPosition());
                add("device fd: seek " + time + " mode " + mode + " callbacks " + callbacks);
            }
        }
        player.seekTo(0, MediaPlayer.SEEK_PREVIOUS_SYNC);
        events.until(2000, "seek complete");

        HandlerThread thread = new HandlerThread("frames");
        thread.start();
        ImageReader reader = ImageReader.newInstance(320, 180, ImageFormat.YUV_420_888, 4);
        AtomicInteger frames = new AtomicInteger();
        reader.setOnImageAvailableListener(r -> {
            Image image = r.acquireNextImage();
            if (image != null) {
                frames.incrementAndGet();
                image.close();
            }
        }, new Handler(thread.getLooper()));
        add("fd: surface " + run(() -> player.setSurface(reader.getSurface())));
        add("fd: start " + run(player::start) + ", playing " + player.isPlaying());
        List<String> played = events.until(20000, "completion", "error");
        add("device fd: callbacks of the playback " + played + ", buffering " + events.buffering);
        // To the device tag: the emulator's playback sometimes stalls (observed, its audio sink).
        add("device fd: the playback " + (played.contains("completion") ? "completed" : "did not complete: " + played)
                + ", playing " + player.isPlaying());
        add("device fd: position after completion " + player.getCurrentPosition() + ", frames " + frames.get());
        add("fd: frames " + (frames.get() > 0 ? "some" : "none"));
        add("fd: start again " + run(player::start) + ", playing " + player.isPlaying());
        add("fd: pause " + run(player::pause) + ", playing " + player.isPlaying());
        add("device fd: position after the pause " + player.getCurrentPosition());
        add("fd: looping " + run(() -> player.setLooping(true)) + " " + player.isLooping() + ", volume "
                + run(() -> player.setVolume(0.5f, 0.25f)) + ", speed 2 "
                + run(() -> player.setPlaybackParams(new PlaybackParams().setSpeed(2f))) + " "
                + answer(() -> player.getPlaybackParams().getSpeed()) + ", playing " + player.isPlaying());
        player.setPlaybackParams(new PlaybackParams().setSpeed(1f));
        // The error callbacks of these calls to the device tag: Android's main thread stays busy after a stop (its
        // handler of the first error reaches the media server), so they come late or not at all, as the reset below
        // drops those not delivered yet (observed on the emulator: once within 300 ms, then after more than 5 s).
        add("fd: stop " + run(player::stop) + ", playing " + player.isPlaying() + ", start " + run(player::start)
                + ", seek " + run(() -> player.seekTo(0)));
        add("fd: prepare after stop " + run(player::prepare) + ", duration " + answer(player::getDuration));
        add("device fd: callbacks after the stop " + events.within(1000));
        player.reset();
        add("fd: after reset duration " + answer(player::getDuration) + ", callbacks " + events.within(300));
        player.release();
        reader.close();
        thread.quitSafely();
    }

    private void metrics(String what, PersistableBundle metrics) {
        if (metrics == null) {
            add(what + " metrics: none");
            return;
        }
        List<String> keys = new ArrayList<>(metrics.keySet());
        Collections.sort(keys);
        for (String key : keys) {
            Object value = metrics.get(key);
            add("device " + what + " metrics " + key + ": "
                    + (value == null ? "null" : value.getClass().getSimpleName() + " " + value));
        }
        add(what + " metrics keys: " + keys);
    }

    private interface Setter {
        void set(MediaPlayer player) throws Exception;
    }

    private void source(String what, Setter setter) {
        source(what, setter, 5000);
    }

    /** A player on the source, prepared synchronously and asynchronously (waiting `ms`): what each answers. */
    private void source(String what, Setter setter, long ms) {
        for (boolean async : new boolean[] {false, true}) {
            MediaPlayer player = new MediaPlayer();
            Events events = new Events(player);
            String set = run(() -> setter.set(player));
            String prepare = "not tried";
            List<String> callbacks;
            if (async) {
                prepare = run(player::prepareAsync);
                callbacks = prepare.equals("ok") ? events.until(ms, "prepared", "error") : events.within(300);
            } else {
                if (set.equals("ok")) prepare = run(player::prepare);
                callbacks = events.within(300);
            }
            add(what + (async ? " async" : "") + ": set " + set + ", prepare " + prepare + ", callbacks " + callbacks
                    + ", duration " + answer(player::getDuration));
            player.release();
        }
    }

    private void sources(Context context, int raw, File copy, byte[] bytes) {
        source("path", player -> player.setDataSource(copy.getPath()));
        source("file Uri", player -> player.setDataSource(context, Uri.fromFile(copy)));
        source("resource Uri", player -> player.setDataSource(context,
                Uri.parse("android.resource://" + context.getPackageName() + "/" + raw)));
        source("FileDescriptor", player -> {
            try (FileInputStream in = new FileInputStream(copy)) {
                player.setDataSource(in.getFD());
            }
        });
        source("FileDescriptor at an offset", player -> {
            try (FileInputStream in = new FileInputStream(copy)) {
                player.setDataSource(in.getFD(), 0, copy.length());
            }
        });
        source("MediaDataSource", player -> player.setDataSource(new Bytes(bytes)));
        source("missing path", player -> player.setDataSource(copy.getPath() + ".missing"));
        byte[] text = new byte[1000];
        java.util.Arrays.fill(text, (byte) 'x');
        source("no movie", player -> player.setDataSource(new Bytes(text)));
        MediaPlayer created = MediaPlayer.create(context, raw);
        add("create: " + (created == null ? "null" : "a player, duration " + created.getDuration() + ", playing "
                + created.isPlaying()));
        if (created != null) created.release();
    }

    /** http and https on a closed port of 127.0.0.1: what the network security config and the permission answer. */
    private void network() {
        source("http", player -> player.setDataSource("http://127.0.0.1:1/baseline.mp4"), 40000);
        source("https", player -> player.setDataSource("https://127.0.0.1:1/baseline.mp4"), 40000);
    }

    /** No DRM scheme: the DRM info and prepareDrm. */
    private void drm(Context context, int raw) throws Exception {
        MediaPlayer player = new MediaPlayer();
        try (AssetFileDescriptor asset = context.getResources().openRawResourceFd(raw)) {
            player.setDataSource(asset);
        }
        player.prepare();
        add("drm: info " + answer(player::getDrmInfo) + ", prepare widevine " + run(() -> player.prepareDrm(WIDEVINE))
                + ", prepare clearkey " + run(() -> player.prepareDrm(CLEARKEY)) + ", release "
                + run(player::releaseDrm));
        player.release();
    }

    /** A MediaDataSource of bytes, -1 past its end as the API asks. */
    private static final class Bytes extends MediaDataSource {
        private final byte[] bytes;

        Bytes(byte[] bytes) {
            this.bytes = bytes;
        }

        @Override
        public int readAt(long position, byte[] buffer, int offset, int size) {
            if (position >= bytes.length) return -1;
            int count = (int) Math.min(size, bytes.length - position);
            System.arraycopy(bytes, (int) position, buffer, offset, count);
            return count;
        }

        @Override
        public long getSize() {
            return bytes.length;
        }

        @Override
        public void close() {}
    }
}
