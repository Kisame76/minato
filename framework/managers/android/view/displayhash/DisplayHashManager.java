// The written part of DisplayHashManager (service inventory, bundle B): one hash algorithm is supported, as on the
// emulator (apk-runner-35) [verified: services-h-test].
import android.content.Context;
import java.util.Collections;
import java.util.Set;

class DisplayHashManager {
    private final Context mContext;

    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public DisplayHashManager(Context context) {
        mContext = context;
    }

    public Set<String> getSupportedHashAlgorithms() {
        return Collections.singleton("PHASH");
    }
}
