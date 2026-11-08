package io.github.kisame76.minato.test.mediabrowser;

import android.content.ComponentName;
import android.content.Context;
import android.media.MediaMetadata;
import android.media.browse.MediaBrowser;
import android.media.session.MediaController;
import android.media.session.MediaSession;
import android.media.session.PlaybackState;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.util.List;

/** A media browser's client of TestBrowserService: connect, browse the root, get an item, then drive its session. */
public class BrowserClient {
    private static final String TAG = MediaBrowserApplication.TAG;
    private final Context mContext;
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private MediaBrowser mBrowser;
    private MediaController mController;

    BrowserClient(Context context) {
        mContext = context;
        final ComponentName service = new ComponentName(context, TestBrowserService.class);
        mBrowser = new MediaBrowser(context, service, new MediaBrowser.ConnectionCallback() {
            @Override
            public void onConnected() {
                Log.i(TAG, "MediaBrowser: onConnected, connected " + mBrowser.isConnected());
                Log.i(TAG, "MediaBrowser: root " + mBrowser.getRoot() + ", service "
                        + mBrowser.getServiceComponent().getShortClassName() + ", extras kind "
                        + (mBrowser.getExtras() == null ? null : mBrowser.getExtras().getString("kind")));
                final MediaSession.Token token = mBrowser.getSessionToken();
                Log.i(TAG, "MediaBrowser: token " + (token != null));
                subscribe();
                mHandler.postDelayed(() -> control(token), 600);
            }

            @Override
            public void onConnectionSuspended() {
                Log.i(TAG, "MediaBrowser: onConnectionSuspended");
            }

            @Override
            public void onConnectionFailed() {
                Log.i(TAG, "MediaBrowser: onConnectionFailed");
                Log.i(TAG, "done");
            }
        }, null);
    }

    private void subscribe() {
        mBrowser.subscribe(mBrowser.getRoot(), new MediaBrowser.SubscriptionCallback() {
            @Override
            public void onChildrenLoaded(String parentId, List<MediaBrowser.MediaItem> children) {
                final StringBuilder out = new StringBuilder();
                for (MediaBrowser.MediaItem item : children) {
                    out.append(item.getMediaId()).append('/').append(item.getDescription().getTitle())
                            .append("/playable ").append(item.isPlayable()).append("/browsable ")
                            .append(item.isBrowsable()).append(' ');
                }
                Log.i(TAG, "MediaBrowser: onChildrenLoaded " + parentId + " " + children.size() + ": " + out);
                mBrowser.getItem("track1", new MediaBrowser.ItemCallback() {
                    @Override
                    public void onItemLoaded(MediaBrowser.MediaItem item) {
                        Log.i(TAG, "MediaBrowser: onItemLoaded " + (item == null ? null : item.getMediaId()));
                    }

                    @Override
                    public void onError(String itemId) {
                        Log.i(TAG, "MediaBrowser: getItem error " + itemId);
                    }
                });
            }

            @Override
            public void onError(String parentId) {
                Log.i(TAG, "MediaBrowser: subscribe error " + parentId);
            }
        });
    }

    /** The controller of the token: reads, the session's state arriving, transport commands arriving. */
    private void control(MediaSession.Token token) {
        mController = new MediaController(mContext, token);
        final MediaSession session = TestBrowserService.sSession;
        Log.i(TAG, "controller: package " + mController.getPackageName() + ", tag " + mController.getTag()
                + ", flags " + mController.getFlags() + ", metadata " + mController.getMetadata()
                + ", state " + mController.getPlaybackState() + ", queue " + mController.getQueue()
                + ", rating type " + mController.getRatingType());
        final MediaController.PlaybackInfo info = mController.getPlaybackInfo();
        Log.i(TAG, "controller: playback info type " + info.getPlaybackType() + ", control "
                + info.getVolumeControl() + ", max " + info.getMaxVolume());
        // The music volume is the device's own.
        Log.i(TAG + "-device", "controller: playback info current " + info.getCurrentVolume());
        Log.i(TAG, "controller: token equals " + token.equals(mController.getSessionToken())
                + ", session token equals " + token.equals(session.getSessionToken()));
        mController.registerCallback(new MediaController.Callback() {
            @Override
            public void onPlaybackStateChanged(PlaybackState state) {
                Log.i(TAG, "controller: onPlaybackStateChanged state " + state.getState() + " position "
                        + state.getPosition());
            }

            @Override
            public void onMetadataChanged(MediaMetadata metadata) {
                Log.i(TAG, "controller: onMetadataChanged " + metadata.getString(MediaMetadata.METADATA_KEY_TITLE));
            }

            @Override
            public void onSessionEvent(String event, Bundle extras) {
                Log.i(TAG, "controller: onSessionEvent " + event);
            }

            @Override
            public void onSessionDestroyed() {
                Log.i(TAG, "controller: onSessionDestroyed");
            }
        });
        session.setActive(true);
        Log.i(TAG, "session: active " + session.isActive());
        session.setMetadata(new MediaMetadata.Builder().putString(MediaMetadata.METADATA_KEY_TITLE, "Song")
                .putLong(MediaMetadata.METADATA_KEY_DURATION, 60000).build());
        session.setPlaybackState(new PlaybackState.Builder().setState(PlaybackState.STATE_PLAYING, 1000, 1f)
                .setActions(PlaybackState.ACTION_PLAY | PlaybackState.ACTION_PAUSE).build());
        session.sendSessionEvent("hello", null);
        mHandler.postDelayed(this::transport, 400);
    }

    private void transport() {
        final MediaController.TransportControls controls = mController.getTransportControls();
        Log.i(TAG, "controller: metadata title " + mController.getMetadata().getString(MediaMetadata.METADATA_KEY_TITLE)
                + ", state " + mController.getPlaybackState().getState());
        controls.play();
        controls.pause();
        controls.seekTo(5000);
        controls.skipToNext();
        final Bundle extras = new Bundle();
        extras.putString("why", "test");
        controls.playFromMediaId("track2", extras);
        controls.sendCustomAction("shuffle", null);
        mHandler.postDelayed(this::finish, 600);
    }

    private void finish() {
        mBrowser.unsubscribe(mBrowser.getRoot());
        TestBrowserService.sSession.release();
        mHandler.postDelayed(() -> {
            mBrowser.disconnect();
            Log.i(TAG, "MediaBrowser: disconnected, connected " + mBrowser.isConnected());
            Log.i(TAG, "done");
        }, 300);
    }

    void start() {
        Log.i(TAG, "MediaBrowser: new, connected " + mBrowser.isConnected());
        mBrowser.connect();
    }
}
