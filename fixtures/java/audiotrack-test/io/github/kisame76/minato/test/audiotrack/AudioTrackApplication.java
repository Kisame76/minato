package io.github.kisame76.minato.test.audiotrack;

import android.app.Application;
import android.media.AudioAttributes;
import android.media.AudioDeviceInfo;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.util.Log;

import java.nio.ByteBuffer;

/**
 * android.media.AudioTrack, on which FMOD's output thread plays (the Unity test APK probe): the minimum buffer sizes and their
 * refusals, tracks from the legacy constructors and the Builder with their checks and states, streaming writes of each
 * kind that fill the buffer, return short, block until there is room or fail, a pause that interrupts a blocked write,
 * flush, stop and release, and a static clip. The data is silence. What the device decides (the sizes, the output's
 * rate) goes to the tag audiotrack-test-device; the lines compared say only how it relates to what was asked. It runs
 * on a thread of its own, so the main thread stays free while the tracks play.
 */
public class AudioTrackApplication extends Application {
    static final String TAG = "audiotrack-test";

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private static void device(String line) {
        Log.i(TAG + "-device", line);
    }

    private interface Action {
        Object run() throws Exception;
    }

    /** What an action returns, or the exception it throws. */
    private static String outcome(Action action) {
        try {
            return String.valueOf(action.run());
        } catch (Exception e) {
            return e.getClass().getName() + ": " + e.getMessage();
        }
    }

    private static void sleep(long millis) {
        try {
            Thread.sleep(millis);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    private static String states(AudioTrack track) {
        return "state " + track.getState() + " play state " + track.getPlayState();
    }

    @Override
    public void onCreate() {
        super.onCreate();
        new Thread(() -> {
            devices();
            minimumSizes();
            refusals();
            streaming();
            interruptedWrite();
            floats();
            byteBuffers();
            builders();
            staticClip();
            released();
            log("done");
        }, "audiotrack-test").start();
    }

    /** The outputs FMOD asks about before it builds its track: Android lists the built-in speaker among them. */
    private void devices() {
        AudioManager manager = getSystemService(AudioManager.class);
        AudioDeviceInfo[] outputs = manager.getDevices(AudioManager.GET_DEVICES_OUTPUTS);
        boolean speaker = false;
        boolean sinks = outputs != null;
        StringBuilder types = new StringBuilder();
        for (AudioDeviceInfo device : outputs == null ? new AudioDeviceInfo[0] : outputs) {
            speaker |= device.getType() == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER && device.getId() > 0;
            sinks &= device.isSink() && !device.isSource();
            types.append(' ').append(device.getType()).append(':').append(device.getProductName());
        }
        log("outputs: listed " + (outputs != null) + ", the built-in speaker " + speaker + ", all sinks " + sinks);
        device("outputs:" + types);
    }

    private static void minimumSizes() {
        int[][] cases = {
            {44100, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT, 4},
            {48000, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT, 4},
            {48000, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT, 2},
            {48000, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_FLOAT, 8},
            {22050, AudioFormat.CHANNEL_CONFIGURATION_MONO, AudioFormat.ENCODING_PCM_8BIT, 1},
            {48000, AudioFormat.CHANNEL_OUT_5POINT1, AudioFormat.ENCODING_PCM_16BIT, 12},
        };
        for (int[] c : cases) {
            int size = AudioTrack.getMinBufferSize(c[0], c[1], c[2]);
            log("min buffer " + c[0] + " " + c[1] + " " + c[2] + ": positive " + (size > 0) + ", whole frames "
                    + (size % c[3] == 0));
            device("min buffer " + c[0] + " " + c[1] + " " + c[2] + ": " + size);
        }
        log("min buffer, rate 1000: " + AudioTrack.getMinBufferSize(1000, AudioFormat.CHANNEL_OUT_STEREO,
                AudioFormat.ENCODING_PCM_16BIT));
        log("min buffer, rate 0: " + AudioTrack.getMinBufferSize(0, AudioFormat.CHANNEL_OUT_STEREO,
                AudioFormat.ENCODING_PCM_16BIT));
        log("min buffer, encoding 99: " + AudioTrack.getMinBufferSize(44100, AudioFormat.CHANNEL_OUT_STEREO, 99));
        log("min buffer, front right alone: " + AudioTrack.getMinBufferSize(44100,
                AudioFormat.CHANNEL_OUT_FRONT_RIGHT, AudioFormat.ENCODING_PCM_16BIT));
        int rate = AudioTrack.getNativeOutputSampleRate(AudioManager.STREAM_MUSIC);
        log("native output rate positive: " + (rate > 0));
        device("native output rate: " + rate);
    }

    private static AudioTrack legacy(int rate, int channels, int encoding, int bytes, int mode) {
        return new AudioTrack(AudioManager.STREAM_MUSIC, rate, channels, encoding, bytes, mode);
    }

    private static void refusals() {
        int stereo = AudioFormat.CHANNEL_OUT_STEREO;
        int pcm16 = AudioFormat.ENCODING_PCM_16BIT;
        log("buffer 0: " + outcome(() -> legacy(44100, stereo, pcm16, 0, AudioTrack.MODE_STREAM)));
        log("buffer 6: " + outcome(() -> legacy(44100, stereo, pcm16, 6, AudioTrack.MODE_STREAM)));
        log("rate 1000: " + outcome(() -> legacy(1000, stereo, pcm16, 4096, AudioTrack.MODE_STREAM)));
        log("encoding 99: " + outcome(() -> legacy(44100, stereo, 99, 4096, AudioTrack.MODE_STREAM)));
        log("mode 5: " + outcome(() -> legacy(44100, stereo, pcm16, 4096, 5)));
        log("front right alone: " + outcome(() -> legacy(44100, AudioFormat.CHANNEL_OUT_FRONT_RIGHT, pcm16, 4096,
                AudioTrack.MODE_STREAM)));
        log("session -5: " + outcome(() -> new AudioTrack(AudioManager.STREAM_MUSIC, 44100, stereo, pcm16, 4096,
                AudioTrack.MODE_STREAM, -5)));
        log("null format: " + outcome(() -> new AudioTrack(new AudioAttributes.Builder().build(), null, 4096,
                AudioTrack.MODE_STREAM, AudioManager.AUDIO_SESSION_ID_GENERATE)));
    }

    private static void streaming() {
        int min = AudioTrack.getMinBufferSize(44100, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT);
        int bytes = min * 2;
        AudioTrack track = legacy(44100, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT, bytes,
                AudioTrack.MODE_STREAM);
        log("legacy track: " + states(track) + ", rate " + track.getSampleRate() + ", channels "
                + track.getChannelCount() + ", encoding " + track.getAudioFormat() + ", configuration "
                + track.getChannelConfiguration() + ", stream type " + track.getStreamType() + ", playback rate "
                + track.getPlaybackRate() + ", head " + track.getPlaybackHeadPosition());
        log("buffer frames at least asked: " + (track.getBufferSizeInFrames() >= bytes / 4) + ", session positive: "
                + (track.getAudioSessionId() > 0));
        device("legacy track: buffer frames " + track.getBufferSizeInFrames() + " for " + bytes / 4 + " asked");
        int capacity = track.getBufferSizeInFrames() * 4;
        byte[] silence = new byte[capacity * 4];
        int first = track.write(silence, 0, silence.length, AudioTrack.WRITE_NON_BLOCKING);
        log("non-blocking write before play: short " + (first > 0 && first < silence.length) + ", whole frames "
                + (first % 4 == 0));
        device("non-blocking write before play: " + first + " of " + silence.length);
        log("non-blocking write when full: " + track.write(silence, 0, 4096, AudioTrack.WRITE_NON_BLOCKING));
        log("write errors: offset -1 " + track.write(silence, -1, 4) + ", past the end "
                + track.write(silence, silence.length - 2, 4) + ", mode 7 " + track.write(silence, 0, 4, 7)
                + ", floats " + track.write(new float[4], 0, 4, AudioTrack.WRITE_BLOCKING));
        log("write of a part frame: " + track.write(silence, 0, 3, AudioTrack.WRITE_NON_BLOCKING));
        log("flush before play: head " + outcome(() -> {
            track.flush();
            return track.getPlaybackHeadPosition();
        }));
        track.play();
        log("play: " + states(track));
        long start = System.nanoTime();
        int written = track.write(silence, 0, silence.length);
        long millis = (System.nanoTime() - start) / 1_000_000;
        log("blocking write of four buffers: all " + (written == silence.length) + ", waited for room "
                + (millis >= 10));
        device("blocking write of four buffers took " + millis + " ms");
        log("shorts: " + track.write(new short[1024], 0, 1024));
        sleep(100);
        log("head moved: " + (track.getPlaybackHeadPosition() > 0));
        log("volume: " + track.setVolume(0.5f) + " " + track.setStereoVolume(2f, -1f));
        track.pause();
        log("pause: " + states(track));
        sleep(150);
        int paused = track.getPlaybackHeadPosition();
        sleep(150);
        log("head still while paused: " + (paused == track.getPlaybackHeadPosition()) + ", positive " + (paused > 0));
        track.flush();
        log("flush while paused: head " + track.getPlaybackHeadPosition() + ", " + states(track));
        track.play();
        log("play after flush: " + states(track) + ", write " + track.write(silence, 0, 4096));
        sleep(100);
        track.stop();
        log("stop: " + states(track) + ", head " + track.getPlaybackHeadPosition());
        log("write after stop: " + track.write(silence, 0, 4096, AudioTrack.WRITE_NON_BLOCKING));
        track.play();
        log("play after stop: " + states(track));
        track.release();
        log("release: " + states(track));
    }

    private static void interruptedWrite() {
        int min = AudioTrack.getMinBufferSize(44100, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT);
        AudioTrack track = legacy(44100, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT, min * 2,
                AudioTrack.MODE_STREAM);
        byte[] silence = new byte[track.getBufferSizeInFrames() * 4 * 40];
        int[] result = {-1000};
        track.play();
        Thread writer = new Thread(() -> result[0] = track.write(silence, 0, silence.length), "writer");
        writer.start();
        sleep(150);
        track.pause();
        try {
            writer.join(3000);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        log("pause during a blocking write: returned " + !writer.isAlive() + ", short "
                + (result[0] >= 0 && result[0] < silence.length) + ", whole frames " + (result[0] % 4 == 0));
        track.release();
        try {
            writer.join(3000);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    private static void floats() {
        AudioTrack track = new AudioTrack(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME).build(),
                new AudioFormat.Builder().setSampleRate(48000).setEncoding(AudioFormat.ENCODING_PCM_FLOAT)
                        .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).build(),
                AudioTrack.getMinBufferSize(48000, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_FLOAT),
                AudioTrack.MODE_STREAM, AudioManager.AUDIO_SESSION_ID_GENERATE);
        log("float track: " + states(track) + ", rate " + track.getSampleRate() + ", encoding "
                + track.getAudioFormat() + ", stream type " + track.getStreamType());
        log("float track's byte and short writes: " + track.write(new byte[8], 0, 8) + " "
                + track.write(new short[8], 0, 8));
        track.play();
        log("float writes: " + track.write(new float[9600], 0, 9600, AudioTrack.WRITE_BLOCKING) + ", bad mode "
                + track.write(new float[8], 0, 8, 3) + ", past the end " + track.write(new float[8], 4, 8,
                AudioTrack.WRITE_BLOCKING));
        track.stop();
        track.release();
    }

    private static void byteBuffers() {
        AudioTrack track = legacy(48000, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT, 9600,
                AudioTrack.MODE_STREAM);
        ByteBuffer direct = ByteBuffer.allocateDirect(4000);
        ByteBuffer heap = ByteBuffer.allocate(4000);
        heap.position(1000);
        int a = track.write(direct, 4000, AudioTrack.WRITE_NON_BLOCKING);
        int b = track.write(heap, 2000, AudioTrack.WRITE_NON_BLOCKING);
        log("byte buffers: direct " + a + " at " + direct.position() + ", heap " + b + " at " + heap.position()
                + ", too big " + track.write(heap, 5000, AudioTrack.WRITE_NON_BLOCKING) + ", mode 9 "
                + track.write(heap, 2, 9));
        track.release();
    }

    private static void builders() {
        AudioTrack plain = outcomeTrack(new AudioTrack.Builder());
        if (plain != null) {
            log("built without a format: " + states(plain) + ", rate is the output's "
                    + (plain.getSampleRate() == AudioTrack.getNativeOutputSampleRate(AudioManager.STREAM_MUSIC))
                    + ", channels " + plain.getChannelCount() + ", encoding " + plain.getAudioFormat()
                    + ", buffer frames positive " + (plain.getBufferSizeInFrames() > 0));
            device("built without a format: rate " + plain.getSampleRate() + ", buffer frames "
                    + plain.getBufferSizeInFrames());
            plain.release();
        }
        log("builder, buffer -1: " + outcome(() -> new AudioTrack.Builder().setBufferSizeInBytes(-1)));
        log("builder, transfer mode 9: " + outcome(() -> new AudioTrack.Builder().setTransferMode(9)));
        log("builder, session 0: " + outcome(() -> new AudioTrack.Builder().setSessionId(0).getClass()
                .getSimpleName()));
        log("builder, null attributes: " + outcome(() -> new AudioTrack.Builder().setAudioAttributes(null)));
        log("builder, buffer 6: " + outcome(() -> new AudioTrack.Builder().setAudioFormat(new AudioFormat.Builder()
                .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).build()).setBufferSizeInBytes(6).build()));
        AudioTrack game = outcomeTrack(new AudioTrack.Builder()
                .setAudioAttributes(new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME)
                        .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build())
                .setAudioFormat(new AudioFormat.Builder().setSampleRate(44100)
                        .setEncoding(AudioFormat.ENCODING_PCM_16BIT).setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                        .build())
                .setBufferSizeInBytes(8192).setTransferMode(AudioTrack.MODE_STREAM));
        if (game != null) {
            log("game track: " + states(game) + ", rate " + game.getSampleRate() + ", stream type "
                    + game.getStreamType());
            game.play();
            log("game track writes: " + game.write(new byte[8192], 0, 8192) + ", " + states(game));
            game.stop();
            game.release();
        }
    }

    private static AudioTrack outcomeTrack(AudioTrack.Builder builder) {
        try {
            return builder.build();
        } catch (RuntimeException e) {
            log("build: " + e.getClass().getName() + ": " + e.getMessage());
            return null;
        }
    }

    private static void staticClip() {
        AudioTrack clip = legacy(44100, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT, 8820,
                AudioTrack.MODE_STATIC);
        log("static clip: " + states(clip) + ", play " + outcome(() -> {
            clip.play();
            return "played";
        }));
        log("static writes: " + clip.write(new byte[8820], 0, 8820) + ", " + states(clip) + ", again "
                + clip.write(new byte[20000], 0, 20000));
        clip.play();
        sleep(400);
        log("static clip after its length: head " + clip.getPlaybackHeadPosition() + ", " + states(clip)
                + ", reload " + clip.reloadStaticData());
        clip.stop();
        log("static clip stopped: head " + clip.getPlaybackHeadPosition() + ", reload " + clip.reloadStaticData()
                + ", head " + clip.getPlaybackHeadPosition());
        clip.release();
    }

    private static void released() {
        AudioTrack track = legacy(44100, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT, 4096,
                AudioTrack.MODE_STREAM);
        track.release();
        log("after release: " + states(track) + ", write " + track.write(new byte[4], 0, 4) + ", volume "
                + track.setVolume(1f) + ", reload " + track.reloadStaticData());
        log("play after release: " + outcome(() -> {
            track.play();
            return "played";
        }));
        log("stop after release: " + outcome(() -> {
            track.stop();
            return "stopped";
        }));
        log("pause after release: " + outcome(() -> {
            track.pause();
            return "paused";
        }));
        log("flush after release: " + outcome(() -> {
            track.flush();
            return "flushed";
        }));
        log("head after release: " + outcome(track::getPlaybackHeadPosition));
        log("release twice: " + outcome(() -> {
            track.release();
            return "released";
        }));
    }
}
