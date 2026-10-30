package io.github.kisame76.minato.test.sdlaudiotrack;

import android.app.Application;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.util.Log;

/**
 * SDL 2.30's Java audio driver (SDLAudioManager.audioOpen, zlib; read, not copied), played with silence: the channel
 * configuration and the encoding from the count and the format, the frame count raised to the minimum buffer size,
 * a MODE_STREAM track on STREAM_MUSIC, play(), the track's rate, encoding and channel count read back, and the audio
 * thread's loop, which writes spec.size bytes again and again in the array's own element type, advances by the count
 * a write returned, sleeps a millisecond on 0 and stops on a negative count. The lines compared say that the values
 * read back are the ones asked for and that the loop was paced by the track at about its rate: the frames written in
 * 2 s are the rate's and the buffer's. The counts go to the tag sdl-audiotrack-test-device.
 */
public class SdlAudioTrackApplication extends Application {
    private static final String TAG = "sdl-audiotrack-test";
    private static final int FRAMES = 1024;

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private static void device(String line) {
        Log.i(TAG + "-device", line);
    }

    private static void sleep(long millis) {
        try {
            Thread.sleep(millis);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    /** One audioOpen and two seconds of the write loop; the encoding is ENCODING_PCM_8BIT, 16BIT or FLOAT. */
    private static void play(int rate, int encoding, int channels, String name) {
        final int sampleSize = encoding == AudioFormat.ENCODING_PCM_8BIT ? 1
                : encoding == AudioFormat.ENCODING_PCM_16BIT ? 2 : 4;
        final int channelConfig = channels == 1 ? AudioFormat.CHANNEL_OUT_MONO : AudioFormat.CHANNEL_OUT_STEREO;
        final int frameSize = sampleSize * channels;
        final int minBufferSize = AudioTrack.getMinBufferSize(rate, channelConfig, encoding);
        final int desiredFrames = Math.max(FRAMES, (minBufferSize + frameSize - 1) / frameSize);
        final AudioTrack track = new AudioTrack(AudioManager.STREAM_MUSIC, rate, channelConfig, encoding,
                desiredFrames * frameSize, AudioTrack.MODE_STREAM);
        log(name + ": state initialized: " + (track.getState() == AudioTrack.STATE_INITIALIZED));
        if (track.getState() != AudioTrack.STATE_INITIALIZED) {
            return;
        }
        track.play();
        log(name + ": read back: rate " + track.getSampleRate() + ", encoding " + track.getAudioFormat() + ", channels "
                + track.getChannelCount());
        device(name + ": min buffer size " + minBufferSize + " bytes, desired frames " + desiredFrames);
        final short[] shorts = new short[FRAMES * channels];
        final byte[] bytes = new byte[FRAMES * channels];
        final float[] floats = new float[FRAMES * channels];
        long frames = 0;
        long zeros = 0;
        long calls = 0;
        final long begin = System.nanoTime();
        while (System.nanoTime() - begin < 2_000_000_000L) {
            // audioWriteShortBuffer, audioWriteByteBuffer and audioWriteFloatBuffer: elements, not bytes.
            final int length = shorts.length;
            int i = 0;
            while (i < length && System.nanoTime() - begin < 2_000_000_000L) {
                final int result = encoding == AudioFormat.ENCODING_PCM_16BIT ? track.write(shorts, i, length - i)
                        : encoding == AudioFormat.ENCODING_PCM_8BIT ? track.write(bytes, i, length - i)
                        : track.write(floats, i, length - i, AudioTrack.WRITE_BLOCKING);
                ++calls;
                if (result > 0) {
                    i += result;
                } else if (result == 0) {
                    ++zeros;
                    sleep(1);
                } else {
                    log(name + ": a write failed with " + result);
                    track.stop();
                    track.release();
                    return;
                }
            }
            frames += i / channels;
        }
        final double seconds = (System.nanoTime() - begin) / 1e9;
        final double writtenRate = frames / seconds;
        device(name + ": " + calls + " writes, " + zeros + " of 0, " + frames + " frames in " + seconds + " s = "
                + (long) writtenRate + " frames/s, head " + track.getPlaybackHeadPosition());
        log(name + ": the loop was paced at about the track's rate (at most 30 percent over, 10 under): "
                + (writtenRate > rate * 0.9 && writtenRate < rate * 1.3));
        track.stop();
        track.release();
    }

    @Override
    public void onCreate() {
        super.onCreate();
        new Thread(() -> {
            play(48000, AudioFormat.ENCODING_PCM_16BIT, 2, "48000 Hz 16-bit stereo");
            play(44100, AudioFormat.ENCODING_PCM_16BIT, 2, "44100 Hz 16-bit stereo");
            play(22050, AudioFormat.ENCODING_PCM_16BIT, 1, "22050 Hz 16-bit mono");
            play(48000, AudioFormat.ENCODING_PCM_FLOAT, 2, "48000 Hz float stereo");
            play(22050, AudioFormat.ENCODING_PCM_8BIT, 1, "22050 Hz 8-bit mono");
            log("done");
        }, "sdl-audiotrack").start();
    }
}
