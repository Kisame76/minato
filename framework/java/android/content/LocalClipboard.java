/*
 * The clipboard (ADR-0045 decision 4's services; gap 1 of the compatibility plan): IClipboard of system_server, local
 * in the app's process, which ServiceManager's table gives as "clipboard" and ClipboardManager asks. It keeps the one
 * primary clip of the app and answers as Android 15's ClipboardService does for the app that has the focus [verified:
 * ClipboardService.java at android-15.0.0_r1, clipboard-test on apk-runner-35-bare]: setPrimaryClip refuses a clip
 * without items ("No items"), a clip is kept as a copy and given back as one, clearPrimaryClip drops it, the
 * description, hasPrimaryClip and hasClipboardText answer from it, and every listener is called when the clip changes,
 * not when an empty clipboard is cleared again. A text clip of up to 400 characters is "classified" in the background,
 * as the service does with the text classifier, which ends with a second call of the listeners and the description's
 * status COMPLETE; a clip of any other kind has the status NOT_PERFORMED.
 * Departure (the decision): the clipboard is not shared with iOS. Minato has no UIPasteboard behind it, so an
 * app sees only what it copied itself, no iOS paste banner appears and nothing of the host reaches the guest. Not
 * kept: the clipboard's automatic clearing after an hour, the access check for an app without the focus (the app is
 * always the one in front), the access notifications, the text classifier and the copy into related profiles.
 * Every other call is AIDL's Default answer. Not under com.android.minato, whose classes
 * tools/framework-host-check.sh compiles on the host without android.*.
 */
package android.content;

import android.os.Binder;
import android.os.IBinder;
import android.os.RemoteException;
import android.text.TextUtils;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Executor;
import java.util.concurrent.Executors;

public final class LocalClipboard extends IClipboard.Default {
    /** ClipboardService's DEFAULT_MAX_CLASSIFICATION_LENGTH. */
    private static final int MAX_CLASSIFICATION_LENGTH = 400;
    /** The time the background classification takes here, as the text classifier takes some on Android. */
    private static final int CLASSIFICATION_MS = 100;

    private final Binder mBinder = new Binder();
    /** ClipboardService's worker, which classifies a text clip: here only the second call of the listeners. */
    private final Executor mWorker = Executors.newSingleThreadExecutor(runnable -> {
        Thread thread = new Thread(runnable, "ClipboardWorker");
        thread.setDaemon(true);
        return thread;
    });
    /** The primary clip and the listeners; under the service's lock. */
    private ClipData mPrimaryClip;
    private final List<IOnPrimaryClipChangedListener> mListeners = new ArrayList<>();

    private LocalClipboard() {
        mBinder.attachInterface(this, IClipboard.Stub.DESCRIPTOR);
    }

    /** ServiceManager's maker for "clipboard": the Binder that carries a new service. */
    public static IBinder newBinder() {
        return new LocalClipboard().asBinder();
    }

    @Override
    public IBinder asBinder() {
        return mBinder;
    }

    @Override
    public void setPrimaryClip(ClipData clip, String callingPackage, String attributionTag, int userId,
            int deviceId) {
        if (clip == null || clip.getItemCount() <= 0) {
            throw new IllegalArgumentException("No items");
        }
        change(copy(clip));
    }

    @Override
    public void clearPrimaryClip(String callingPackage, String attributionTag, int userId, int deviceId) {
        change(null);
    }

    @Override
    public ClipData getPrimaryClip(String pkg, String attributionTag, int userId, int deviceId) {
        synchronized (mBinder) {
            return mPrimaryClip == null ? null : copy(mPrimaryClip);
        }
    }

    @Override
    public ClipDescription getPrimaryClipDescription(String callingPackage, String attributionTag, int userId,
            int deviceId) {
        synchronized (mBinder) {
            return mPrimaryClip == null ? null : copy(mPrimaryClip.getDescription());
        }
    }

    @Override
    public boolean hasPrimaryClip(String callingPackage, String attributionTag, int userId, int deviceId) {
        synchronized (mBinder) {
            return mPrimaryClip != null;
        }
    }

    @Override
    public boolean hasClipboardText(String callingPackage, String attributionTag, int userId, int deviceId) {
        synchronized (mBinder) {
            if (mPrimaryClip == null) {
                return false;
            }
            CharSequence text = mPrimaryClip.getItemAt(0).getText();
            return text != null && text.length() > 0;
        }
    }

    @Override
    public void addPrimaryClipChangedListener(IOnPrimaryClipChangedListener listener, String callingPackage,
            String attributionTag, int userId, int deviceId) {
        synchronized (mBinder) {
            for (IOnPrimaryClipChangedListener known : mListeners) {
                if (known.asBinder() == listener.asBinder()) {
                    return;
                }
            }
            mListeners.add(listener);
        }
    }

    @Override
    public void removePrimaryClipChangedListener(IOnPrimaryClipChangedListener listener, String callingPackage,
            String attributionTag, int userId, int deviceId) {
        synchronized (mBinder) {
            mListeners.removeIf(known -> known.asBinder() == listener.asBinder());
        }
    }

    /** ClipboardService.setPrimaryClipInternalNoClassifyLocked: a change, told to every listener. */
    private void change(ClipData clip) {
        boolean classify = false;
        synchronized (mBinder) {
            if (clip == null && mPrimaryClip == null) {
                return;
            }
            mPrimaryClip = clip;
            if (clip != null) {
                // startClassificationLocked: a text clip of up to 400 characters is classified in the background, which
                // ends with a second call of the listeners; any other clip is not classified.
                CharSequence text = clip.getItemAt(0).getText();
                classify = !TextUtils.isEmpty(text) && text.length() <= MAX_CLASSIFICATION_LENGTH;
                clip.getDescription().setClassificationStatus(classify ? ClipDescription.CLASSIFICATION_NOT_COMPLETE
                        : ClipDescription.CLASSIFICATION_NOT_PERFORMED);
            }
        }
        tellListeners();
        if (classify) {
            final ClipData classified = clip;
            mWorker.execute(() -> {
                // The classification takes a while on Android; a clip that is replaced meanwhile is not told twice.
                try {
                    Thread.sleep(CLASSIFICATION_MS);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    return;
                }
                synchronized (mBinder) {
                    if (mPrimaryClip != classified) {
                        return;
                    }
                    classified.getDescription().setClassificationStatus(ClipDescription.CLASSIFICATION_COMPLETE);
                }
                tellListeners();
            });
        }
    }

    /**
     * What Binder gives the other side, a copy, as ClipboardService copies a clip into a related profile: the clip
     * with an item of its own for each, and the description with the classification status. (Not through a parcel,
     * which would ask for IntentSender, a gap of gap 9.)
     */
    private static ClipData copy(ClipData clip) {
        ClipData out = new ClipData(clip);
        for (int i = clip.getItemCount() - 1; i >= 0; i--) {
            out.setItemAt(i, new ClipData.Item(clip.getItemAt(i)));
        }
        out.getDescription().setClassificationStatus(clip.getDescription().getClassificationStatus());
        return out;
    }

    private static ClipDescription copy(ClipDescription description) {
        ClipDescription out = new ClipDescription(description);
        out.setClassificationStatus(description.getClassificationStatus());
        return out;
    }

    private void tellListeners() {
        List<IOnPrimaryClipChangedListener> told;
        synchronized (mBinder) {
            told = new ArrayList<>(mListeners);
        }
        for (IOnPrimaryClipChangedListener listener : told) {
            try {
                listener.dispatchPrimaryClipChanged();
            } catch (RemoteException e) {
                // A listener of this process cannot be gone.
            }
        }
    }
}
