package android.os.ext;

import android.os.SystemProperties;
import java.util.Collections;
import java.util.HashMap;
import java.util.Map;

/**
 * The versions of the SDK extensions, as Android 15's SdkExtensions answers them: the extension versions the system
 * image carries, read from the build.version.extensions.* properties (13 each when a property is not set, as the
 * emulator image of Android 15 answers), 0 for a number that is no extension.
 */
public final class SdkExtensions {
    /** The extension of the Android Ad Services module. */
    public static final int AD_SERVICES = 1_000_000;

    /** The version Android 15's emulator image reports for each extension it has (apk-runner-35, 2026-11-03). */
    private static final int CURRENT = 13;

    private static final Map<Integer, Integer> sVersions = versions();

    private SdkExtensions() {
    }

    private static Map<Integer, Integer> versions() {
        Map<Integer, Integer> map = new HashMap<>();
        map.put(30, SystemProperties.getInt("build.version.extensions.r", CURRENT));
        map.put(31, SystemProperties.getInt("build.version.extensions.s", CURRENT));
        map.put(33, SystemProperties.getInt("build.version.extensions.t", CURRENT));
        map.put(34, SystemProperties.getInt("build.version.extensions.u", CURRENT));
        map.put(35, SystemProperties.getInt("build.version.extensions.v", CURRENT));
        map.put(AD_SERVICES, SystemProperties.getInt("build.version.extensions.ad_services", CURRENT));
        return Collections.unmodifiableMap(map);
    }

    public static int getExtensionVersion(int extension) {
        if (extension < 30) {
            throw new IllegalArgumentException("not a valid extension: " + extension);
        }
        Integer version = sVersions.get(extension);
        return version == null ? 0 : version;
    }

    public static Map<Integer, Integer> getAllExtensionVersions() {
        return sVersions;
    }
}
