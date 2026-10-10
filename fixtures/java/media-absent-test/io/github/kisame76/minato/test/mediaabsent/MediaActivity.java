package io.github.kisame76.minato.test.mediaabsent;

import android.app.Activity;
import android.content.Context;
import android.graphics.PixelFormat;
import android.media.ImageReader;
import android.media.MediaCodecList;
import android.media.MediaCrypto;
import android.media.MediaDrm;
import android.media.MediaExtractor;
import android.media.MediaMetadataRetriever;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import java.util.Arrays;
import java.util.List;
import java.util.UUID;

/**
 * The Unity test APK gap sort's category 2 test: android.media's classes that need hardware or native parts Minato lacks. Its
 * parts run one after the other from the resumed activity: DRM and crypto objects for a scheme that no device has, the
 * codec list, the metadata retriever and the extractor without a source, the image reader and the TV input service.
 * Each part logs with the tag media-absent-test what the classes answer on every device; what depends on the device's
 * codecs, DRM plugins and cameras has the tag media-absent-test-device and is only shown. A part that throws logs
 * "failed" and the next one runs, so that every run reaches "done".
 */
public class MediaActivity extends Activity {
    static final String TAG = "media-absent-test";
    static final String DEVICE = TAG + "-device";
    /** A scheme UUID that no DRM plugin has. */
    static final UUID NONE = new UUID(0L, 0L);
    static final UUID WIDEVINE = UUID.fromString("edef8ba9-79d6-4ace-a3c8-27dcd51d21ed");
    static final UUID CLEARKEY = UUID.fromString("e2719d58-a985-b3c9-781a-b030af78d30e");

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private boolean mStarted;

    private interface Part {
        void run() throws Exception;
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (!mStarted) {
            mStarted = true;
            mHandler.post(this::runParts);
        }
    }

    private void runParts() {
        List<Part> parts = Arrays.asList(this::drm, this::crypto, this::codecs, this::retriever, this::extractor,
                this::images, this::tv);
        for (Part part : parts) {
            try {
                part.run();
            } catch (Exception | LinkageError e) {
                Log.i(TAG, "failed: " + e.getClass().getName() + ": " + e.getMessage());
                Log.i(DEVICE, Log.getStackTraceString(e));
            }
        }
        Log.i(TAG, "done");
    }

    /** The exception's class and its message's first line: Android's DRM and crypto errors go on with a plugin log. */
    private static String thrown(Throwable e) {
        String message = e.getMessage();
        int line = message == null ? -1 : message.indexOf('\n');
        return e.getClass().getName() + ": " + (line < 0 ? message : message.substring(0, line));
    }

    /** MediaDrm for a scheme without a plugin; the plugins of the device are only shown. */
    private void drm() {
        Log.i(TAG, "drm none supported: " + MediaDrm.isCryptoSchemeSupported(NONE));
        Log.i(TAG, "drm none with mime: " + MediaDrm.isCryptoSchemeSupported(NONE, "video/avc"));
        Log.i(TAG, "drm schemes list: " + (MediaDrm.getSupportedCryptoSchemes() != null));
        try {
            new MediaDrm(NONE).close();
            Log.i(TAG, "drm none: created");
        } catch (Exception e) {
            Log.i(TAG, "drm none: " + thrown(e));
        }
        Log.i(DEVICE, "drm widevine " + MediaDrm.isCryptoSchemeSupported(WIDEVINE) + ", clearkey "
                + MediaDrm.isCryptoSchemeSupported(CLEARKEY) + ", schemes " + MediaDrm.getSupportedCryptoSchemes());
    }

    /** MediaCrypto for the same scheme. */
    private void crypto() {
        Log.i(TAG, "crypto none supported: " + MediaCrypto.isCryptoSchemeSupported(NONE));
        try {
            new MediaCrypto(NONE, new byte[0]).release();
            Log.i(TAG, "crypto none: created");
        } catch (Exception e) {
            Log.i(TAG, "crypto none: " + thrown(e));
        }
        Log.i(DEVICE, "crypto widevine " + MediaCrypto.isCryptoSchemeSupported(WIDEVINE));
    }

    /** The codec list: what it holds is the device's, that it can be made is not. */
    private void codecs() {
        new MediaCodecList(MediaCodecList.REGULAR_CODECS);
        Log.i(TAG, "codec list: created");
        Log.i(DEVICE, "codec count " + MediaCodecList.getCodecCount());
    }

    /** The retriever before it has a source, and with a file that is not there. */
    private void retriever() throws Exception {
        MediaMetadataRetriever retriever = new MediaMetadataRetriever();
        try {
            Log.i(TAG, "retriever without a source: " + retriever.extractMetadata(
                    MediaMetadataRetriever.METADATA_KEY_DURATION));
        } catch (Exception e) {
            Log.i(TAG, "retriever without a source: " + thrown(e));
        }
        try {
            retriever.setDataSource("/nonexistent/file.mp4");
            Log.i(TAG, "retriever with a missing file: no error");
        } catch (Exception e) {
            Log.i(TAG, "retriever with a missing file: " + e.getClass().getName());
        }
        retriever.release();
        Log.i(TAG, "retriever released");
    }

    /** The extractor before it has a source, and with a file that is not there. */
    private void extractor() throws Exception {
        MediaExtractor extractor = new MediaExtractor();
        try {
            Log.i(TAG, "extractor tracks without a source: " + extractor.getTrackCount());
        } catch (Exception e) {
            Log.i(TAG, "extractor tracks without a source: " + thrown(e));
        }
        try {
            extractor.setDataSource("/nonexistent/file.mp4");
            Log.i(TAG, "extractor with a missing file: no error");
        } catch (Exception e) {
            Log.i(TAG, "extractor with a missing file: " + e.getClass().getName());
        }
        extractor.release();
        Log.i(TAG, "extractor released");
    }

    /** The image reader, which Android makes over a buffer queue; Minato's is only shown. */
    private void images() {
        try {
            ImageReader reader = ImageReader.newInstance(64, 48, PixelFormat.RGBA_8888, 2);
            Log.i(DEVICE, "image reader " + reader.getWidth() + "x" + reader.getHeight() + ", max "
                    + reader.getMaxImages());
            reader.close();
        } catch (Exception e) {
            Log.i(DEVICE, "image reader: " + thrown(e));
        }
        Log.i(TAG, "image reader asked");
    }

    /** The TV input service: a phone has none. */
    private void tv() {
        Object manager = getSystemService(Context.TV_INPUT_SERVICE);
        Log.i(DEVICE, "tv input service " + (manager == null ? "null" : manager.getClass().getName()));
        Log.i(TAG, "tv input service asked");
    }
}
