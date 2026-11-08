package io.github.kisame76.minato.test.mediabrowser;

import android.media.MediaDescription;
import android.media.browse.MediaBrowser;
import android.media.session.MediaSession;
import android.os.Bundle;
import android.service.media.MediaBrowserService;
import android.util.Log;

import java.util.ArrayList;
import java.util.List;

/** The browser service of the test: a root "root" with two playable children, and a session as media apps keep. */
public class TestBrowserService extends MediaBrowserService {
    private static final String TAG = MediaBrowserApplication.TAG;
    /** The service's session, which the app's other code sets state on. */
    static MediaSession sSession;

    @Override
    public void onCreate() {
        super.onCreate();
        sSession = new MediaSession(this, "media-browser-test");
        sSession.setFlags(MediaSession.FLAG_HANDLES_MEDIA_BUTTONS | MediaSession.FLAG_HANDLES_TRANSPORT_CONTROLS);
        sSession.setCallback(new MediaSession.Callback() {
            @Override
            public void onPlay() {
                Log.i(TAG, "session: onPlay");
            }

            @Override
            public void onPause() {
                Log.i(TAG, "session: onPause");
            }

            @Override
            public void onSeekTo(long pos) {
                Log.i(TAG, "session: onSeekTo " + pos);
            }

            @Override
            public void onSkipToNext() {
                Log.i(TAG, "session: onSkipToNext");
            }

            @Override
            public void onPlayFromMediaId(String mediaId, Bundle extras) {
                Log.i(TAG, "session: onPlayFromMediaId " + mediaId + " " + (extras == null ? null
                        : extras.getString("why")));
            }

            @Override
            public void onCustomAction(String action, Bundle extras) {
                Log.i(TAG, "session: onCustomAction " + action);
            }
        });
        setSessionToken(sSession.getSessionToken());
        Log.i(TAG, "service: created, token set: " + (getSessionToken() != null) + ", session active "
                + sSession.isActive());
    }

    @Override
    public BrowserRoot onGetRoot(String clientPackageName, int clientUid, Bundle rootHints) {
        Log.i(TAG, "service: onGetRoot for " + clientPackageName);
        final Bundle extras = new Bundle();
        extras.putString("kind", "test");
        return new BrowserRoot("root", extras);
    }

    @Override
    public void onLoadChildren(String parentId, Result<List<MediaBrowser.MediaItem>> result) {
        Log.i(TAG, "service: onLoadChildren(" + parentId + ")");
        final List<MediaBrowser.MediaItem> items = new ArrayList<>();
        if ("root".equals(parentId)) {
            for (int n = 1; n <= 2; n++) {
                final MediaDescription description = new MediaDescription.Builder().setMediaId("track" + n)
                        .setTitle("Track " + n).setSubtitle("Sub " + n).build();
                items.add(new MediaBrowser.MediaItem(description, MediaBrowser.MediaItem.FLAG_PLAYABLE));
            }
        }
        result.sendResult(items);
    }

    @Override
    public void onLoadItem(String itemId, Result<MediaBrowser.MediaItem> result) {
        Log.i(TAG, "service: onLoadItem(" + itemId + ")");
        final MediaDescription description = new MediaDescription.Builder().setMediaId(itemId).setTitle("Item")
                .build();
        result.sendResult(new MediaBrowser.MediaItem(description, MediaBrowser.MediaItem.FLAG_PLAYABLE));
    }

    @Override
    public void onDestroy() {
        Log.i(TAG, "service: onDestroy");
        if (sSession != null) sSession.release();
        sSession = null;
        super.onDestroy();
    }
}
