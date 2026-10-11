package io.github.kisame76.minato.test.mediafields;

import android.app.Application;
import android.util.Log;

/**
 * Reflection over android.media's classes, as Unity's audio and video paths and the SDKs behind them use it (the Unity test APK
 * gap sort, category 3m): every class below by name, then its declared fields, declared constructors, declared methods
 * with their parameter and return types, and its public methods, each readable or the error it gives. A type that a
 * member names and the framework lacks makes the read throw NoClassDefFoundError (ART resolves every member's
 * signature). Which members a class has differs by device and is not logged, only whether they can be read.
 */
public class MediaFieldsApplication extends Application {
    static final String TAG = "media-fields-test";

    private static final String[] CLASSES = {
        "android.media.AudioTrack", "android.media.AudioManager", "android.media.AudioFormat",
        "android.media.AudioAttributes", "android.media.AudioDeviceInfo", "android.media.AudioFocusRequest",
        "android.media.AudioTimestamp", "android.media.PlaybackParams", "android.media.VolumeShaper",
        "android.media.VolumeShaper$Configuration", "android.media.VolumeShaper$Operation",
        "android.media.AudioPresentation", "android.media.MediaCodecList", "android.media.MediaCodecInfo",
        "android.media.MediaCodecInfo$CodecCapabilities", "android.media.MediaCodecInfo$CodecProfileLevel",
        "android.media.MediaCodecInfo$VideoCapabilities", "android.media.MediaCodecInfo$AudioCapabilities",
        "android.media.MediaCodec", "android.media.MediaCodec$CryptoInfo", "android.media.MediaCodec$BufferInfo",
        "android.media.MediaFormat", "android.media.MediaExtractor", "android.media.DrmInitData",
        "android.media.MediaDrm", "android.media.MediaCrypto", "android.media.MediaMetadataRetriever",
        "android.media.ImageReader", "android.media.metrics.LogSessionId",
    };

    private static String verdict(Class<?> c) {
        String fields;
        try {
            c.getDeclaredFields();
            fields = "fields readable";
        } catch (Throwable t) {
            fields = "fields " + t.getClass().getName();
        }
        String constructors;
        try {
            c.getDeclaredConstructors();
            constructors = "constructors readable";
        } catch (Throwable t) {
            constructors = "constructors " + t.getClass().getName();
        }
        String declared;
        try {
            for (java.lang.reflect.Method m : c.getDeclaredMethods()) {
                m.getParameterTypes();
                m.getReturnType();
            }
            declared = "declared methods readable";
        } catch (Throwable t) {
            declared = "declared methods " + t.getClass().getName();
        }
        String pub;
        try {
            c.getMethods();
            pub = "public methods readable";
        } catch (Throwable t) {
            pub = "public methods " + t.getClass().getName();
        }
        return fields + ", " + constructors + ", " + declared + ", " + pub;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        for (String name : CLASSES) {
            try {
                Log.i(TAG, name + ": " + verdict(Class.forName(name, false, getClassLoader())));
            } catch (Throwable t) {
                Log.i(TAG, name + ": " + t.getClass().getName());
            }
        }
        Log.i(TAG, "done");
    }
}
