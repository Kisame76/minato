// The written part of FileIntegrityManager (service inventory, bundle B): fs-verity is supported, as on the emulator
// (apk-runner-35) [verified: services-h-test].
import android.content.Context;

class FileIntegrityManager {
    private final Context mContext;

    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public FileIntegrityManager(Context context) {
        mContext = context;
    }

    public boolean isApkVeritySupported() {
        return true;
    }
}
