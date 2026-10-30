/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * The ringtone player (ADR-0060 e2e1's RingtoneManager step): SystemUI's RingtonePlayer.mCallback at
 * android-15.0.0_r1 (packages/SystemUI/src/com/android/systemui/media/RingtonePlayer.java), in the app's process, which
 * AudioManager.getRingtonePlayer gives as the audio service gives SystemUI's. Ringtone plays a sound it cannot open
 * itself through it, as on Android, where a device always has one: each token's Ringtone of its own (allowRemote
 * false) plays, stops and answers isPlaying. Its caller is always the app: playAsync and stopAsync, the system's
 * only, are refused as SystemUI refuses them, and logged, as a oneway call's exception never reaches its caller.
 * Taken as it is but for a user's context (the app's own) and the async player, which only the system UID reaches.
 */
package android.media;

import android.annotation.Nullable;
import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.Binder;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.os.RemoteException;
import android.os.UserHandle;
import android.provider.MediaStore;
import android.util.Log;

import java.io.IOException;
import java.util.HashMap;

/** @hide */
public final class LocalRingtonePlayer extends IRingtonePlayer.Stub {
    private static final String TAG = "RingtonePlayer";

    private static LocalRingtonePlayer sInstance;

    private final Context mContext;
    private final HashMap<IBinder, Client> mClients = new HashMap<>();

    private LocalRingtonePlayer(Context context) {
        mContext = context;
    }

    /** The process's player, as the system's one SystemUI. */
    static synchronized LocalRingtonePlayer get(Context context) {
        if (sInstance == null) {
            Context app = context.getApplicationContext();
            sInstance = new LocalRingtonePlayer(app != null ? app : context);
        }
        return sInstance;
    }

    private class Client implements IBinder.DeathRecipient {
        private final IBinder mToken;
        private final Ringtone mRingtone;

        Client(IBinder token, Uri uri, AudioAttributes aa,
                @Nullable VolumeShaper.Configuration volumeShaperConfig) {
            mToken = token;

            mRingtone = new Ringtone(mContext, false);
            mRingtone.setAudioAttributesField(aa);
            mRingtone.setUri(uri, volumeShaperConfig);
            mRingtone.createLocalMediaPlayer();
        }

        @Override
        public void binderDied() {
            synchronized (mClients) {
                mClients.remove(mToken);
            }
            mRingtone.stop();
        }
    }

    @Override
    public void play(IBinder token, Uri uri, AudioAttributes aa, float volume, boolean looping)
            throws RemoteException {
        playWithVolumeShaping(token, uri, aa, volume, looping, null);
    }

    @Override
    public void playWithVolumeShaping(IBinder token, Uri uri, AudioAttributes aa, float volume,
            boolean looping, @Nullable VolumeShaper.Configuration volumeShaperConfig)
            throws RemoteException {
        Client client;
        synchronized (mClients) {
            client = mClients.get(token);
            if (client == null) {
                client = new Client(token, uri, aa, volumeShaperConfig);
                token.linkToDeath(client, 0);
                mClients.put(token, client);
            }
        }
        client.mRingtone.setLooping(looping);
        client.mRingtone.setVolume(volume);
        client.mRingtone.play();
    }

    @Override
    public void stop(IBinder token) {
        Client client;
        synchronized (mClients) {
            client = mClients.remove(token);
        }
        if (client != null) {
            client.mToken.unlinkToDeath(client, 0);
            client.mRingtone.stop();
        }
    }

    @Override
    public boolean isPlaying(IBinder token) {
        Client client;
        synchronized (mClients) {
            client = mClients.get(token);
        }
        if (client != null) {
            return client.mRingtone.isPlaying();
        } else {
            return false;
        }
    }

    @Override
    public void setPlaybackProperties(IBinder token, float volume, boolean looping,
            boolean hapticGeneratorEnabled) {
        Client client;
        synchronized (mClients) {
            client = mClients.get(token);
        }
        if (client != null) {
            client.mRingtone.setVolume(volume);
            client.mRingtone.setLooping(looping);
            client.mRingtone.setHapticGeneratorEnabled(hapticGeneratorEnabled);
        }
        // else no client for token when setting playback properties but will be set at play()
    }

    @Override
    public void playAsync(Uri uri, UserHandle user, boolean looping, AudioAttributes aa,
            float volume) {
        if (Binder.getCallingUid() != Process.SYSTEM_UID) {
            Log.w(TAG, new SecurityException("Async playback only available from system UID."));
        }
    }

    @Override
    public void stopAsync() {
        if (Binder.getCallingUid() != Process.SYSTEM_UID) {
            Log.w(TAG, new SecurityException("Async playback only available from system UID."));
        }
    }

    @Override
    public String getTitle(Uri uri) {
        return Ringtone.getTitle(mContext, uri,
                false /*followSettingsUri*/, false /*allowRemote*/);
    }

    @Override
    public ParcelFileDescriptor openRingtone(Uri uri) {
        final ContentResolver resolver = mContext.getContentResolver();

        // Only open the requested Uri if it's a well-known ringtone or
        // other sound from the platform media store, otherwise this opens
        // up arbitrary access to any file on external storage.
        if (uri.toString().startsWith(MediaStore.Audio.Media.EXTERNAL_CONTENT_URI.toString())) {
            try (Cursor c = resolver.query(uri, new String[] {
                    MediaStore.Audio.AudioColumns.IS_RINGTONE,
                    MediaStore.Audio.AudioColumns.IS_ALARM,
                    MediaStore.Audio.AudioColumns.IS_NOTIFICATION
            }, null, null, null)) {
                if (c.moveToFirst()) {
                    if (c.getInt(0) != 0 || c.getInt(1) != 0 || c.getInt(2) != 0) {
                        try {
                            return resolver.openFileDescriptor(uri, "r");
                        } catch (IOException e) {
                            throw new SecurityException(e);
                        }
                    }
                }
            }
        }
        throw new SecurityException("Uri is not ringtone, alarm, or notification: " + uri);
    }
}
