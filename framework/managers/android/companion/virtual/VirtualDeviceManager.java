// The written part of VirtualDeviceManager (service inventory, bundle B): there are no virtual devices and no persistent
// ids [verified: services-h-test].
import android.content.Context;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

class VirtualDeviceManager {
    private final Context mContext;

    /** @hide PackageManager's name of the default device; javac inlines it, so it carries AOSP's value. */
    public static final String PERSISTENT_DEVICE_ID_DEFAULT = "default:0";

    /** @hide SystemSensorManager's, for a virtual device's context. */
    public static final String ACTION_VIRTUAL_DEVICE_REMOVED =
            "android.companion.virtual.action.VIRTUAL_DEVICE_REMOVED";

    /** @hide */
    public static final String EXTRA_VIRTUAL_DEVICE_ID = "android.companion.virtual.extra.VIRTUAL_DEVICE_ID";

    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public VirtualDeviceManager(Context context) {
        mContext = context;
    }

    public List<VirtualDevice> getVirtualDevices() {
        return new ArrayList<>();
    }

    /** @hide The policy of the default device is the default policy, as no virtual device has another. */
    public int getDevicePolicy(int deviceId, int policyType) {
        return 0;
    }

    /** @hide TextToSpeech's, for a virtual device's audio session: no session of its own, so a generated one. */
    public int getAudioPlaybackSessionId(int deviceId) {
        return 0;
    }

    /** @hide */
    public Set<String> getAllPersistentDeviceIds() {
        return new HashSet<>();
    }
}
