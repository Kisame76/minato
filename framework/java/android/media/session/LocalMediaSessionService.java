/*
 * The media session service: ISessionManager of system_server, local in the
 * app's process, which ServiceManager's table gives as "media_session" and AOSP's MediaSessionManager asks. It makes
 * the sessions of the app's own MediaSession objects: a session with a controller binder, so that MediaSession has its
 * Token, MediaController its binder and MediaBrowserService its session token. Transport commands of a controller go to
 * the session's callback, state changes of the session to its controllers' callbacks, in the process. What system_server
 * adds is not here, as no other app's sessions are visible to an app on Android 15 either: no session list, no media
 * key dispatch, no remote volume; those calls are AIDL's Default answer.
 */
package android.media.session;

import android.app.PendingIntent;
import android.content.Intent;
import android.content.pm.ParceledListSlice;
import android.media.AudioAttributes;
import android.media.MediaMetadata;
import android.media.Rating;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Process;
import android.os.RemoteException;
import android.os.ResultReceiver;
import android.view.KeyEvent;

import java.util.ArrayList;
import java.util.List;

public final class LocalMediaSessionService extends ISessionManager.Default {
    private final Binder mBinder = new Binder();

    private LocalMediaSessionService() {
        mBinder.attachInterface(this, ISessionManager.Stub.DESCRIPTOR);
    }

    /** ServiceManager's maker for "media_session": the Binder that carries a new service. */
    public static IBinder newBinder() {
        return new LocalMediaSessionService().asBinder();
    }

    @Override
    public IBinder asBinder() {
        return mBinder;
    }

    @Override
    public ISession createSession(String packageName, ISessionCallback sessionCb, String tag, Bundle sessionInfo,
            int userId) {
        return new Session(packageName, sessionCb, tag, sessionInfo);
    }

    /** One session: ISession for its MediaSession, with the state its controller reads. */
    private static final class Session extends ISession.Default {
        private final Binder mSessionBinder = new Binder();
        final String mPackageName;
        final ISessionCallback mCallback;
        final String mTag;
        final Bundle mSessionInfo;
        final Controller mController = new Controller(this);
        final List<ISessionControllerCallback> mControllerCallbacks = new ArrayList<>();
        final Binder mQueueBinder;
        long mFlags;
        boolean mActive;
        boolean mDestroyed;
        MediaMetadata mMetadata;
        PlaybackState mPlaybackState;
        List<MediaSession.QueueItem> mQueue;
        CharSequence mQueueTitle;
        Bundle mExtras;
        int mRatingType;
        PendingIntent mLaunchIntent;
        MediaController.PlaybackInfo mPlaybackInfo;

        Session(String packageName, ISessionCallback callback, String tag, Bundle sessionInfo) {
            mPackageName = packageName;
            mCallback = callback;
            mTag = tag;
            mSessionInfo = sessionInfo;
            mSessionBinder.attachInterface(this, ISession.Stub.DESCRIPTOR);
            mQueueBinder = new ParcelableListBinder<>(MediaSession.QueueItem.class, list -> {
                synchronized (this) {
                    mQueue = list;
                }
                each(cb -> cb.onQueueChanged(list == null ? null : new ParceledListSlice<>(list)));
            });
            // A local session's volume is the music stream's, as MediaSessionRecord reports it.
            int max = 15, current = 0;
            final android.app.Application app = android.app.ActivityThread.currentApplication();
            final android.media.AudioManager audio = app == null ? null
                    : app.getSystemService(android.media.AudioManager.class);
            if (audio != null) {
                max = audio.getStreamMaxVolume(android.media.AudioManager.STREAM_MUSIC);
                current = audio.getStreamVolume(android.media.AudioManager.STREAM_MUSIC);
            }
            mPlaybackInfo = new MediaController.PlaybackInfo(MediaController.PlaybackInfo.PLAYBACK_TYPE_LOCAL,
                    android.media.VolumeProvider.VOLUME_CONTROL_ABSOLUTE, max, current,
                    new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).build(), null);
        }

        interface Each {
            void call(ISessionControllerCallback callback) throws RemoteException;
        }

        void each(Each each) {
            final List<ISessionControllerCallback> copy;
            synchronized (this) {
                copy = new ArrayList<>(mControllerCallbacks);
            }
            for (ISessionControllerCallback callback : copy) {
                try {
                    each.call(callback);
                } catch (RemoteException e) {
                    // A callback of this process does not die.
                }
            }
        }

        @Override
        public IBinder asBinder() {
            return mSessionBinder;
        }

        @Override
        public void sendEvent(String event, Bundle data) {
            each(cb -> cb.onEvent(event, data));
        }

        @Override
        public ISessionController getController() {
            return mController;
        }

        @Override
        public void setFlags(int flags) {
            mFlags = flags;
        }

        @Override
        public void setActive(boolean active) {
            mActive = active;
        }

        @Override
        public void setLaunchPendingIntent(PendingIntent pi) {
            mLaunchIntent = pi;
        }

        @Override
        public void destroySession() {
            synchronized (this) {
                // As MediaSessionRecord.destroy: a session that is destroyed tells its controllers once.
                if (mDestroyed) return;
                mDestroyed = true;
            }
            each(cb -> cb.onSessionDestroyed());
        }

        @Override
        public void setMetadata(MediaMetadata metadata, long duration, String metadataDescription) {
            synchronized (this) {
                mMetadata = metadata;
            }
            each(cb -> cb.onMetadataChanged(metadata));
        }

        @Override
        public void setPlaybackState(PlaybackState state) {
            synchronized (this) {
                mPlaybackState = state;
            }
            each(cb -> cb.onPlaybackStateChanged(state));
        }

        @Override
        public void resetQueue() {
            synchronized (this) {
                mQueue = null;
            }
            each(cb -> cb.onQueueChanged(null));
        }

        @Override
        public IBinder getBinderForSetQueue() {
            return mQueueBinder;
        }

        @Override
        public void setQueueTitle(CharSequence title) {
            synchronized (this) {
                mQueueTitle = title;
            }
            each(cb -> cb.onQueueTitleChanged(title));
        }

        @Override
        public void setExtras(Bundle extras) {
            synchronized (this) {
                mExtras = extras;
            }
            each(cb -> cb.onExtrasChanged(extras));
        }

        @Override
        public void setRatingType(int type) {
            mRatingType = type;
        }

        @Override
        public void setPlaybackToLocal(AudioAttributes attributes) {
            final MediaController.PlaybackInfo info = new MediaController.PlaybackInfo(
                    MediaController.PlaybackInfo.PLAYBACK_TYPE_LOCAL,
                    android.media.VolumeProvider.VOLUME_CONTROL_ABSOLUTE, 15, mPlaybackInfo.getCurrentVolume(),
                    attributes != null ? attributes : mPlaybackInfo.getAudioAttributes(), null);
            mPlaybackInfo = info;
            each(cb -> cb.onVolumeInfoChanged(info));
        }

        @Override
        public void setPlaybackToRemote(int control, int max, String controlId) {
            final MediaController.PlaybackInfo info = new MediaController.PlaybackInfo(
                    MediaController.PlaybackInfo.PLAYBACK_TYPE_REMOTE, control, max, mPlaybackInfo.getCurrentVolume(),
                    mPlaybackInfo.getAudioAttributes(), controlId);
            mPlaybackInfo = info;
            each(cb -> cb.onVolumeInfoChanged(info));
        }

        @Override
        public void setCurrentVolume(int currentVolume) {
            final MediaController.PlaybackInfo info = new MediaController.PlaybackInfo(
                    mPlaybackInfo.getPlaybackType(), mPlaybackInfo.getVolumeControl(),
                    mPlaybackInfo.getMaxVolume(), currentVolume, mPlaybackInfo.getAudioAttributes(),
                    mPlaybackInfo.getVolumeControlId());
            mPlaybackInfo = info;
            each(cb -> cb.onVolumeInfoChanged(info));
        }
    }

    /** One session's controller: ISessionController over the session's state and its callback. */
    private static final class Controller extends ISessionController.Default {
        private final Binder mBinder = new Binder();
        private final Session mSession;

        Controller(Session session) {
            mSession = session;
            mBinder.attachInterface(this, ISessionController.Stub.DESCRIPTOR);
        }

        @Override
        public IBinder asBinder() {
            return mBinder;
        }

        private static int pid() {
            return Process.myPid();
        }

        private static int uid() {
            return Process.myUid();
        }

        @Override
        public void sendCommand(String packageName, String command, Bundle args, ResultReceiver cb) {
            try {
                mSession.mCallback.onCommand(packageName, pid(), uid(), command, args, cb);
            } catch (RemoteException e) {
                // In this process, a callback does not die.
            }
        }

        @Override
        public boolean sendMediaButton(String packageName, KeyEvent mediaButton) {
            try {
                final Intent intent = new Intent(Intent.ACTION_MEDIA_BUTTON).putExtra(Intent.EXTRA_KEY_EVENT,
                        mediaButton);
                mSession.mCallback.onMediaButtonFromController(packageName, pid(), uid(), intent);
                return true;
            } catch (RemoteException e) {
                return false;
            }
        }

        @Override
        public void registerCallback(String packageName, ISessionControllerCallback cb) {
            synchronized (mSession) {
                mSession.mControllerCallbacks.add(cb);
            }
        }

        @Override
        public void unregisterCallback(ISessionControllerCallback cb) {
            synchronized (mSession) {
                mSession.mControllerCallbacks.remove(cb);
            }
        }

        @Override
        public String getPackageName() {
            return mSession.mPackageName;
        }

        @Override
        public String getTag() {
            return mSession.mTag;
        }

        @Override
        public Bundle getSessionInfo() {
            return mSession.mSessionInfo;
        }

        @Override
        public PendingIntent getLaunchPendingIntent() {
            return mSession.mLaunchIntent;
        }

        @Override
        public long getFlags() {
            return mSession.mFlags;
        }

        @Override
        public MediaController.PlaybackInfo getVolumeAttributes() {
            return mSession.mPlaybackInfo;
        }

        @Override
        public MediaMetadata getMetadata() {
            synchronized (mSession) {
                return mSession.mMetadata;
            }
        }

        @Override
        public PlaybackState getPlaybackState() {
            synchronized (mSession) {
                return mSession.mPlaybackState;
            }
        }

        @Override
        public ParceledListSlice<MediaSession.QueueItem> getQueue() {
            synchronized (mSession) {
                return mSession.mQueue == null ? null : new ParceledListSlice<>(mSession.mQueue);
            }
        }

        @Override
        public CharSequence getQueueTitle() {
            synchronized (mSession) {
                return mSession.mQueueTitle;
            }
        }

        @Override
        public Bundle getExtras() {
            synchronized (mSession) {
                return mSession.mExtras;
            }
        }

        @Override
        public int getRatingType() {
            return mSession.mRatingType;
        }

        /** A transport command as the session's callback takes it. */
        private interface Command {
            void run(ISessionCallback callback, String packageName) throws RemoteException;
        }

        private void send(String packageName, Command command) {
            try {
                command.run(mSession.mCallback, packageName);
            } catch (RemoteException e) {
                // In this process, a callback does not die.
            }
        }

        @Override
        public void prepare(String packageName) {
            send(packageName, (cb, p) -> cb.onPrepare(p, pid(), uid()));
        }

        @Override
        public void prepareFromMediaId(String packageName, String mediaId, Bundle extras) {
            send(packageName, (cb, p) -> cb.onPrepareFromMediaId(p, pid(), uid(), mediaId, extras));
        }

        @Override
        public void prepareFromSearch(String packageName, String query, Bundle extras) {
            send(packageName, (cb, p) -> cb.onPrepareFromSearch(p, pid(), uid(), query, extras));
        }

        @Override
        public void prepareFromUri(String packageName, Uri uri, Bundle extras) {
            send(packageName, (cb, p) -> cb.onPrepareFromUri(p, pid(), uid(), uri, extras));
        }

        @Override
        public void play(String packageName) {
            send(packageName, (cb, p) -> cb.onPlay(p, pid(), uid()));
        }

        @Override
        public void playFromMediaId(String packageName, String mediaId, Bundle extras) {
            send(packageName, (cb, p) -> cb.onPlayFromMediaId(p, pid(), uid(), mediaId, extras));
        }

        @Override
        public void playFromSearch(String packageName, String query, Bundle extras) {
            send(packageName, (cb, p) -> cb.onPlayFromSearch(p, pid(), uid(), query, extras));
        }

        @Override
        public void playFromUri(String packageName, Uri uri, Bundle extras) {
            send(packageName, (cb, p) -> cb.onPlayFromUri(p, pid(), uid(), uri, extras));
        }

        @Override
        public void skipToQueueItem(String packageName, long id) {
            send(packageName, (cb, p) -> cb.onSkipToTrack(p, pid(), uid(), id));
        }

        @Override
        public void pause(String packageName) {
            send(packageName, (cb, p) -> cb.onPause(p, pid(), uid()));
        }

        @Override
        public void stop(String packageName) {
            send(packageName, (cb, p) -> cb.onStop(p, pid(), uid()));
        }

        @Override
        public void next(String packageName) {
            send(packageName, (cb, p) -> cb.onNext(p, pid(), uid()));
        }

        @Override
        public void previous(String packageName) {
            send(packageName, (cb, p) -> cb.onPrevious(p, pid(), uid()));
        }

        @Override
        public void fastForward(String packageName) {
            send(packageName, (cb, p) -> cb.onFastForward(p, pid(), uid()));
        }

        @Override
        public void rewind(String packageName) {
            send(packageName, (cb, p) -> cb.onRewind(p, pid(), uid()));
        }

        @Override
        public void seekTo(String packageName, long pos) {
            send(packageName, (cb, p) -> cb.onSeekTo(p, pid(), uid(), pos));
        }

        @Override
        public void rate(String packageName, Rating rating) {
            send(packageName, (cb, p) -> cb.onRate(p, pid(), uid(), rating));
        }

        @Override
        public void setPlaybackSpeed(String packageName, float speed) {
            send(packageName, (cb, p) -> cb.onSetPlaybackSpeed(p, pid(), uid(), speed));
        }

        @Override
        public void sendCustomAction(String packageName, String action, Bundle args) {
            send(packageName, (cb, p) -> cb.onCustomAction(p, pid(), uid(), action, args));
        }

        @Override
        public void adjustVolume(String packageName, String opPackageName, int direction, int flags) {
            send(packageName, (cb, p) -> cb.onAdjustVolume(p, pid(), uid(), direction));
        }

        @Override
        public void setVolumeTo(String packageName, String opPackageName, int value, int flags) {
            send(packageName, (cb, p) -> cb.onSetVolumeTo(p, pid(), uid(), value));
        }
    }
}
