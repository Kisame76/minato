// The written part of DrmManagerClient (services batch C): the OMA DRM client, of which Minato has no engine. AOSP
// asks native engines, of which an emulator has none either: it handles no content, knows no object type (UNKNOWN,
// -1) and lists no engine [verified: services-c-test]. The argument checks are AOSP's, so that a wrong call fails as
// it does there.
import android.content.Context;
import android.net.Uri;

import java.util.ArrayList;
import java.util.Collection;
import java.util.concurrent.atomic.AtomicBoolean;

class DrmManagerClient {
    /** What the native engine answers when no plug-in knows the object, not DrmObjectType.UNKNOWN (0). */
    private static final int NO_ENGINE = -1;

    private final AtomicBoolean mClosed = new AtomicBoolean();

    public DrmManagerClient(Context context) {
    }

    public void close() {
        mClosed.set(true);
    }

    public void release() {
        close();
    }

    public String[] getAvailableDrmEngines() {
        return new String[0];
    }

    public Collection<DrmSupportInfo> getAvailableDrmSupportInfo() {
        return new ArrayList<>();
    }

    public boolean canHandle(String path, String mimeType) {
        if ((null == path || path.equals("")) && (null == mimeType || mimeType.equals(""))) {
            throw new IllegalArgumentException("Path or the mimetype should be non null");
        }
        return false;
    }

    public boolean canHandle(Uri uri, String mimeType) {
        if ((null == uri || Uri.EMPTY.equals(uri)) && (null == mimeType || mimeType.equals(""))) {
            throw new IllegalArgumentException("Uri or the mimetype should be non null");
        }
        return false;
    }

    public int getDrmObjectType(String path, String mimeType) {
        if ((null == path || path.equals("")) && (null == mimeType || mimeType.equals(""))) {
            throw new IllegalArgumentException("Path or the mimetype should be non null");
        }
        return NO_ENGINE;
    }

    public int getDrmObjectType(Uri uri, String mimeType) {
        if ((null == uri || Uri.EMPTY.equals(uri)) && (null == mimeType || mimeType.equals(""))) {
            throw new IllegalArgumentException("Uri or the mimetype should be non null");
        }
        return NO_ENGINE;
    }
}
