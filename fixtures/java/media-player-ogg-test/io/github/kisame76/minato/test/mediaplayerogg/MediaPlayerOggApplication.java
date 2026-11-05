package io.github.kisame76.minato.test.mediaplayerogg;

import android.app.Application;
import android.content.Context;
import android.content.res.AssetFileDescriptor;
import android.media.MediaDataSource;
import android.media.MediaFormat;
import android.media.MediaPlayer;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

/**
 * ADR-0060 e2's test of android.media.MediaPlayer on Ogg Vorbis assets, as libGDX plays its music (an asset's file
 * descriptor, prepare, looping, start): tone.ogg (stereo, 22050 Hz, 5568 samples) and long.ogg (stereo, 44100 Hz,
 * about 1.2 s), both stored in the APK, and broken.ogg (an Ogg page's capture pattern and nothing a reader takes). On a
 * thread that Application.attachBaseContext starts, whose players' callbacks come on the main thread, it logs with the
 * tag media-player-ogg-test:
 * - each file prepared from its descriptor: duration, video size, position, tracks;
 * - a playback to its end and what the player answers then;
 * - looping across the end (no completion while looping, the position within the file), then the end once looping
 *   is off;
 * - seeks while paused and the positions they give, and a short playback from one;
 * - broken.ogg's prepare;
 * - long.ogg and tone.ogg from a MediaDataSource of their bytes, as an app plays music it holds in memory (tone.ogg's
 *   with an unknown size, -1): prepared, a playback to its end, a seek, and the source closed by release.
 * Times that depend on the device's audio go to the tag with "-device" (lines that start with "device "). It ends
 * with "done".
 */
public class MediaPlayerOggApplication extends Application {
    private static final String TAG = "media-player-ogg-test";

    private final List<String> lines = new ArrayList<>();

    @Override
    protected void attachBaseContext(Context base) {
        super.attachBaseContext(base);
        new Thread(() -> {
            try {
                test(base);
            } catch (Throwable t) {
                add("failed: " + t);
                add("device " + Log.getStackTraceString(t));
            }
            add("done");
            flush();
        }, TAG).start();
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
                    Log.i(TAG + "-device", part.startsWith("device ") ? part.substring(7) : part);
                } else {
                    Log.i(TAG, part);
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

        Events(MediaPlayer player) {
            player.setOnPreparedListener(p -> queue.add("prepared"));
            player.setOnCompletionListener(p -> queue.add("completion"));
            player.setOnSeekCompleteListener(p -> queue.add("seek complete"));
            player.setOnErrorListener((p, what, extra) -> {
                queue.add("error " + what + " " + extra);
                return true;
            });
            player.setOnInfoListener((p, what, extra) -> {
                queue.add("info " + what + " " + extra);
                return false;
            });
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
        played(context, "tone.ogg");
        played(context, "long.ogg");
        looped(context, "tone.ogg");
        sought(context, "long.ogg");
        broken(context);
        fromMemory(context, "long.ogg", true);
        fromMemory(context, "tone.ogg", false);
    }

    /** A player with the asset's descriptor, as libGDX's AndroidMusic sets it, and its prepare's callbacks. */
    private MediaPlayer prepared(Context context, String name, Events[] events) throws Exception {
        MediaPlayer player = new MediaPlayer();
        events[0] = new Events(player);
        try (AssetFileDescriptor asset = context.getAssets().openFd(name)) {
            add(name + ": set " + run(() -> player.setDataSource(asset.getFileDescriptor(), asset.getStartOffset(),
                    asset.getLength())));
        }
        add(name + ": prepare " + run(player::prepare) + ", callbacks " + events[0].until(5000, "prepared", "error"));
        return player;
    }

    /** The file prepared, its tracks, a playback to its end. */
    private void played(Context context, String name) throws Exception {
        Events[] events = new Events[1];
        MediaPlayer player = prepared(context, name, events);
        add(name + ": duration " + answer(player::getDuration) + ", size " + player.getVideoWidth() + "x"
                + player.getVideoHeight() + ", position " + answer(player::getCurrentPosition) + ", playing "
                + player.isPlaying() + ", looping " + player.isLooping());
        MediaPlayer.TrackInfo[] tracks = player.getTrackInfo();
        add(name + ": " + tracks.length + " tracks");
        for (int index = 0; index < tracks.length; index++) {
            MediaPlayer.TrackInfo track = tracks[index];
            MediaFormat format = track.getFormat();
            add(name + " track " + index + ": type " + track.getTrackType() + ", language " + track.getLanguage()
                    + ", format " + (format == null ? "none" : format.getString(MediaFormat.KEY_MIME)));
        }
        add(name + ": selected audio "
                + answer(() -> player.getSelectedTrack(MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_AUDIO)));
        add(name + ": start " + run(player::start) + ", playing " + player.isPlaying());
        List<String> callbacks = events[0].until(10000, "completion", "error");
        add(name + ": " + (callbacks.contains("completion") ? "completed" : "did not complete: " + callbacks)
                + ", playing " + player.isPlaying());
        // To the device tag: the emulator's audio sink leaves a short file's position at 0 (observed: tone.ogg's).
        add("device " + name + ": callbacks of the playback " + callbacks + ", position at the end "
                + player.getCurrentPosition() + " of " + player.getDuration());
        add(name + ": start again " + run(player::start) + ", playing " + player.isPlaying());
        callbacks = events[0].until(10000, "completion", "error");
        add(name + ": " + (callbacks.contains("completion") ? "completed again" : "did not complete: " + callbacks));
        player.release();
    }

    /** Looping across the end: no completion, the position within the file, then the end once looping is off. */
    private void looped(Context context, String name) throws Exception {
        Events[] events = new Events[1];
        MediaPlayer player = prepared(context, name, events);
        final int duration = player.getDuration();
        add(name + " looping: set " + run(() -> player.setLooping(true)) + ", looping " + player.isLooping()
                + ", start " + run(player::start));
        // Four of tone.ogg's lengths and more: the player passes its end at least three times.
        List<String> callbacks = events[0].within(1200);
        int position = player.getCurrentPosition();
        add(name + " looping: callbacks after 1200 ms " + callbacks + ", playing " + player.isPlaying()
                + ", position within the file " + (position >= 0 && position <= duration));
        add("device " + name + " looping: position " + position + " of " + duration);
        add(name + " looping: off " + run(() -> player.setLooping(false)) + ", looping " + player.isLooping());
        callbacks = events[0].until(5000, "completion", "error");
        add(name + " looping: " + (callbacks.contains("completion") ? "completed" : "did not complete: " + callbacks)
                + ", playing " + player.isPlaying());
        player.release();
    }

    /** Seeks while paused, and the positions they give. */
    private void sought(Context context, String name) throws Exception {
        Events[] events = new Events[1];
        MediaPlayer player = prepared(context, name, events);
        for (int time : new int[] {600, 0, 1100, 250}) {
            String set = run(() -> player.seekTo(time));
            List<String> callbacks = events[0].until(5000, "seek complete", "error");
            add(name + ": seek " + time + " " + set + ", callbacks " + callbacks + ", position "
                    + player.getCurrentPosition());
        }
        add(name + ": start " + run(player::start));
        pause(300);
        add(name + ": pause " + run(player::pause) + ", playing " + player.isPlaying());
        int position = player.getCurrentPosition();
        // How far it got to the device tag: the emulator's audio sink had not moved after 300 ms (observed).
        add(name + ": position after 300 ms from 250 before the end " + (position >= 250
                && position < player.getDuration()));
        add("device " + name + ": position after 300 ms from 250: " + position);
        player.release();
    }

    /** An app's source of bytes in memory, with its size or with -1 (unknown), which says when it is closed. */
    private static final class MemorySource extends MediaDataSource {
        final byte[] bytes;
        final boolean sized;
        volatile boolean closed;

        MemorySource(byte[] bytes, boolean sized) {
            this.bytes = bytes;
            this.sized = sized;
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
            return sized ? bytes.length : -1;
        }

        @Override
        public void close() {
            closed = true;
        }
    }

    private static byte[] read(Context context, String name) throws Exception {
        try (InputStream in = context.getAssets().open(name)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buffer = new byte[16384];
            for (int got; (got = in.read(buffer)) > 0; ) out.write(buffer, 0, got);
            return out.toByteArray();
        }
    }

    /** The file from a MediaDataSource of its bytes: prepared, played to its end, a seek, then released. */
    private void fromMemory(Context context, String name, boolean sized) throws Exception {
        String label = name + " in memory" + (sized ? "" : " of unknown size");
        MediaPlayer player = new MediaPlayer();
        Events events = new Events(player);
        MemorySource source = new MemorySource(read(context, name), sized);
        add(label + ": set " + run(() -> player.setDataSource(source)));
        add(label + ": prepare " + run(player::prepare) + ", callbacks " + events.until(5000, "prepared", "error"));
        add(label + ": duration " + answer(player::getDuration) + ", position " + answer(player::getCurrentPosition));
        MediaPlayer.TrackInfo[] tracks = player.getTrackInfo();
        add(label + ": " + tracks.length + " tracks");
        for (int index = 0; index < tracks.length; index++) {
            add(label + " track " + index + ": type " + tracks[index].getTrackType() + ", language "
                    + tracks[index].getLanguage());
        }
        add(label + ": start " + run(player::start) + ", playing " + player.isPlaying());
        List<String> callbacks = events.until(10000, "completion", "error");
        add(label + ": " + (callbacks.contains("completion") ? "completed" : "did not complete: " + callbacks)
                + ", playing " + player.isPlaying());
        String set = run(() -> player.seekTo(100));
        add(label + ": seek 100 " + set + ", callbacks " + events.until(5000, "seek complete", "error")
                + ", position " + player.getCurrentPosition());
        add(label + ": closed before release " + source.closed);
        player.release();
        pause(500);
        add(label + ": closed 500 ms after release " + source.closed);
    }

    /** An Ogg capture pattern and nothing a reader takes. */
    private void broken(Context context) throws Exception {
        Events[] events = new Events[1];
        MediaPlayer player = prepared(context, "broken.ogg", events);
        add("broken.ogg: duration " + answer(player::getDuration) + ", callbacks " + events[0].within(500));
        player.release();
    }
}
