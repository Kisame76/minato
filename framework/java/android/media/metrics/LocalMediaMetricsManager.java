/*
 * The media metrics service: IMediaMetricsManager of system_server, local in
 * the app's process, which ServiceManager's table gives as "media_metrics" and AOSP's MediaMetricsManager asks;
 * media3's MediaMetricsListener takes a playback session from it on Android 12 and later. Each session's ID is 12
 * random bytes in URL-safe Base64 without padding, 16 characters, as MediaMetricsManagerService makes them
 * [verified: the IDs of system-services-test on apk-runner-35, 16 characters with "_"]. The reports go nowhere, as
 * Android's go nowhere an app can read: the service passes them on to statsd. Every other call is AIDL's Default
 * answer.
 */
package android.media.metrics;

import android.os.Binder;
import android.os.IBinder;
import android.util.Base64;

import java.security.SecureRandom;

public final class LocalMediaMetricsManager extends IMediaMetricsManager.Default {
    private final Binder mBinder = new Binder();
    private final SecureRandom mRandom = new SecureRandom();

    private LocalMediaMetricsManager() {
        mBinder.attachInterface(this, IMediaMetricsManager.Stub.DESCRIPTOR);
    }

    /** ServiceManager's maker for "media_metrics": the Binder that carries a new service. */
    public static IBinder newBinder() {
        return new LocalMediaMetricsManager().asBinder();
    }

    @Override
    public IBinder asBinder() {
        return mBinder;
    }

    /** A new session's ID, as MediaMetricsManagerService makes it. */
    private String sessionId() {
        final byte[] id = new byte[12];
        mRandom.nextBytes(id);
        return Base64.encodeToString(id, Base64.NO_PADDING | Base64.NO_WRAP | Base64.URL_SAFE);
    }

    @Override
    public String getPlaybackSessionId(int userId) {
        return sessionId();
    }

    @Override
    public String getRecordingSessionId(int userId) {
        return sessionId();
    }

    @Override
    public String getTranscodingSessionId(int userId) {
        return sessionId();
    }

    @Override
    public String getEditingSessionId(int userId) {
        return sessionId();
    }

    @Override
    public String getBundleSessionId(int userId) {
        return sessionId();
    }
}
