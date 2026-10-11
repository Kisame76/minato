package io.github.kisame76.minato.test.mediaformat;

import android.app.Application;
import android.media.MediaFormat;
import android.util.Log;
import java.nio.ByteBuffer;
import java.util.Arrays;
import java.util.TreeSet;

/**
 * android.media.MediaFormat, the key-value map that apps and SDKs fill with a codec's or a track's parameters (the
 * The Unity test APK gap sort, category 3m): the formats the factories make, every value type set and read back, the defaults, the
 * exceptions of a missing key and of a wrong type, features, the copy constructor and some constants. Each part logs
 * with the tag media-format-test; a part that throws logs "failed" and the next one runs, so every run reaches "done".
 */
public class MediaFormatApplication extends Application {
    static final String TAG = "media-format-test";

    private interface Part {
        void run() throws Exception;
    }

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private static String thrown(Throwable t) {
        return t.getClass().getName();
    }

    private void factories() {
        MediaFormat video = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, 1280, 720);
        log("video: " + video);
        log("video mime " + video.getString(MediaFormat.KEY_MIME) + ", " + video.getInteger(MediaFormat.KEY_WIDTH)
                + "x" + video.getInteger(MediaFormat.KEY_HEIGHT));
        MediaFormat audio = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_AAC, 44100, 2);
        log("audio: " + audio);
        MediaFormat subtitle = MediaFormat.createSubtitleFormat(MediaFormat.MIMETYPE_TEXT_VTT, "en");
        log("subtitle: " + subtitle);
        log("empty: " + new MediaFormat());
    }

    private void values() {
        MediaFormat f = new MediaFormat();
        f.setInteger("int", 7);
        f.setLong("long", 1L << 40);
        f.setFloat("float", 1.5f);
        f.setString("string", "text");
        ByteBuffer buffer = ByteBuffer.wrap(new byte[] {1, 2, 3, 4});
        f.setByteBuffer("buffer", buffer);
        log("int " + f.getInteger("int") + ", long " + f.getLong("long") + ", float " + f.getFloat("float")
                + ", string " + f.getString("string"));
        ByteBuffer back = f.getByteBuffer("buffer");
        log("buffer " + back.remaining() + " bytes, first " + back.get(0));
        log("defaults " + f.getInteger("missing", 9) + ", " + f.getLong("missing", 10L) + ", "
                + f.getFloat("missing", 2.5f) + ", " + f.getString("missing", "dflt") + " "
                + f.getByteBuffer("missing") + " " + f.getString("missing"));
        log("number " + f.getNumber("int") + ", " + f.getNumber("float") + ", " + f.getNumber("missing", 3));
        log("contains " + f.containsKey("int") + " " + f.containsKey("missing"));
        log("types " + f.getValueTypeForKey("int") + " " + f.getValueTypeForKey("long") + " "
                + f.getValueTypeForKey("float") + " " + f.getValueTypeForKey("string") + " "
                + f.getValueTypeForKey("buffer") + " " + f.getValueTypeForKey("missing"));
        log("keys " + new TreeSet<>(f.getKeys()));
        f.setInteger("int", 8);
        log("replaced " + f.getInteger("int"));
        f.removeKey("int");
        log("removed " + f.containsKey("int") + ", keys " + new TreeSet<>(f.getKeys()));
        f.setString("string", null);
        log("null string " + f.containsKey("string") + " " + f.getString("string"));
    }

    private void errors() {
        MediaFormat f = new MediaFormat();
        f.setInteger("int", 7);
        f.setString("string", "text");
        try {
            f.getInteger("missing");
            log("missing int: no error");
        } catch (Throwable t) {
            log("missing int: " + thrown(t));
        }
        try {
            f.getString("int");
            log("int as string: no error");
        } catch (Throwable t) {
            log("int as string: " + thrown(t));
        }
        try {
            f.getInteger("string");
            log("string as int: no error");
        } catch (Throwable t) {
            log("string as int: " + thrown(t));
        }
        try {
            log("int as long " + f.getLong("int", 5L));
        } catch (Throwable t) {
            log("int as long: " + thrown(t));
        }
        try {
            log("int as float " + f.getFloat("int", 5f));
        } catch (Throwable t) {
            log("int as float: " + thrown(t));
        }
        try {
            f.getKeys().add("x");
            log("keys add: no error");
        } catch (Throwable t) {
            log("keys add: " + thrown(t));
        }
    }

    private void features() {
        MediaFormat f = new MediaFormat();
        log("features before " + f.containsFeature("secure-playback"));
        try {
            log("feature unset " + f.getFeatureEnabled("secure-playback"));
        } catch (Throwable t) {
            log("feature unset: " + thrown(t) + ": " + t.getMessage());
        }
        f.setFeatureEnabled("secure-playback", true);
        f.setFeatureEnabled("tunneled-playback", false);
        log("features " + f.getFeatureEnabled("secure-playback") + " " + f.getFeatureEnabled("tunneled-playback") + " "
                + f.containsFeature("secure-playback") + " " + f.containsFeature("other"));
        log("feature keys " + new TreeSet<>(f.getKeys()));
        try {
            f.getFeatureEnabled("other");
            log("feature missing: no error");
        } catch (Throwable t) {
            log("feature missing: " + thrown(t));
        }
    }

    private void copies() {
        MediaFormat a = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_HEVC, 640, 480);
        MediaFormat b = new MediaFormat(a);
        b.setInteger(MediaFormat.KEY_WIDTH, 320);
        log("copy " + a.getInteger(MediaFormat.KEY_WIDTH) + " " + b.getInteger(MediaFormat.KEY_WIDTH) + " "
                + b.getString(MediaFormat.KEY_MIME));
        try {
            new MediaFormat((MediaFormat) null);
            log("copy of null: no error");
        } catch (Throwable t) {
            log("copy of null: " + thrown(t));
        }
    }

    private void constants() {
        log("constants " + MediaFormat.KEY_BIT_RATE + " " + MediaFormat.KEY_FRAME_RATE + " "
                + MediaFormat.KEY_SAMPLE_RATE + " " + MediaFormat.KEY_CHANNEL_COUNT + " " + MediaFormat.KEY_DURATION
                + " " + MediaFormat.MIMETYPE_AUDIO_OPUS + " " + MediaFormat.MIMETYPE_VIDEO_VP9);
        log("types " + MediaFormat.TYPE_NULL + MediaFormat.TYPE_INTEGER + MediaFormat.TYPE_LONG + MediaFormat.TYPE_FLOAT
                + MediaFormat.TYPE_STRING + MediaFormat.TYPE_BYTE_BUFFER + " color "
                + MediaFormat.COLOR_RANGE_FULL + MediaFormat.COLOR_STANDARD_BT709 + MediaFormat.COLOR_TRANSFER_HLG);
    }

    @Override
    public void onCreate() {
        super.onCreate();
        for (Part part : Arrays.<Part>asList(this::factories, this::values, this::errors, this::features,
                this::copies, this::constants)) {
            try {
                part.run();
            } catch (Throwable t) {
                log("failed: " + thrown(t) + ": " + t.getMessage());
            }
        }
        log("done");
    }
}
