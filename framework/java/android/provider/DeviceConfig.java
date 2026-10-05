/*
 * Ours (ADR-0043 step d)): android.provider.DeviceConfig under AOSP's name, the ConfigInfrastructure module's class,
 * which is not fetched. Android's reads the flags a server pushes to the device; Minato's device gets none, so every
 * flag has the default its caller passes, as on a device whose DeviceConfig holds no override. The text classifier's
 * settings read it at every text change (TextClassificationConstants), and ActivityThread's core settings at the start
 * (CoreSettingsObserver's entries). Writing a flag takes WRITE_DEVICE_CONFIG, which no app holds. Settings.Config
 * takes its names from here too, which ADR-0045's gap 13a had as a compile-only stub: the sync modes, with Android 15's
 * values, BadConfigException and MonitorCallback [verified: ConfigInfrastructure at android-15.0.0_r1].
 */
package android.provider;

import java.util.Collections;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.Executor;

/** @hide */
public final class DeviceConfig {
    public static final String NAMESPACE_TEXTCLASSIFIER = "textclassifier";
    public static final String NAMESPACE_WIDGET = "widget";

    public static final int SYNC_DISABLED_MODE_NONE = 0;
    public static final int SYNC_DISABLED_MODE_PERSISTENT = 1;
    public static final int SYNC_DISABLED_MODE_UNTIL_REBOOT = 2;

    private DeviceConfig() {
    }

    public static String getProperty(String namespace, String name) {
        return null;
    }

    public static Properties getProperties(String namespace, String... names) {
        return new Properties(namespace, Collections.emptyMap());
    }

    public static String getString(String namespace, String name, String defaultValue) {
        return defaultValue;
    }

    public static boolean getBoolean(String namespace, String name, boolean defaultValue) {
        return defaultValue;
    }

    public static int getInt(String namespace, String name, int defaultValue) {
        return defaultValue;
    }

    public static long getLong(String namespace, String name, long defaultValue) {
        return defaultValue;
    }

    public static float getFloat(String namespace, String name, float defaultValue) {
        return defaultValue;
    }

    /** No flag changes on Minato, so the listener is never called. */
    public static void addOnPropertiesChangedListener(String namespace, Executor executor,
            OnPropertiesChangedListener onPropertiesChangedListener) {
    }

    public static void removeOnPropertiesChangedListener(
            OnPropertiesChangedListener onPropertiesChangedListener) {
    }

    /** A snapshot of a namespace's flags: none on Minato. */
    public static class Properties {
        private final String mNamespace;
        private final Map<String, String> mMap;

        public Properties(String namespace, Map<String, String> keyValueMap) {
            mNamespace = namespace;
            mMap = keyValueMap == null ? Collections.emptyMap() : keyValueMap;
        }

        public String getNamespace() {
            return mNamespace;
        }

        public Set<String> getKeyset() {
            return mMap.keySet();
        }

        public String getString(String name, String defaultValue) {
            final String value = mMap.get(name);
            return value != null ? value : defaultValue;
        }

        public boolean getBoolean(String name, boolean defaultValue) {
            final String value = mMap.get(name);
            return value != null ? Boolean.parseBoolean(value) : defaultValue;
        }

        public int getInt(String name, int defaultValue) {
            final String value = mMap.get(name);
            try {
                return value != null ? Integer.parseInt(value) : defaultValue;
            } catch (NumberFormatException e) {
                return defaultValue;
            }
        }

        public long getLong(String name, long defaultValue) {
            final String value = mMap.get(name);
            try {
                return value != null ? Long.parseLong(value) : defaultValue;
            } catch (NumberFormatException e) {
                return defaultValue;
            }
        }

        public float getFloat(String name, float defaultValue) {
            final String value = mMap.get(name);
            try {
                return value != null ? Float.parseFloat(value) : defaultValue;
            } catch (NumberFormatException e) {
                return defaultValue;
            }
        }
    }

    /** As AOSP's: what hears of a namespace's changed flags. */
    public interface OnPropertiesChangedListener {
        void onPropertiesChanged(Properties properties);
    }

    /** As AOSP's: what Settings.Config.setStrings throws when the provider cannot set a namespace's flags. */
    public static class BadConfigException extends Exception {
        private static final long serialVersionUID = 1L;
    }

    /** As AOSP's: what hears of the device config's updates and accesses, which Settings.Config can register. */
    public interface MonitorCallback {
        void onNamespaceUpdate(String updatedNamespace);

        void onDeviceConfigAccess(String callingPackage, String namespace);
    }
}
