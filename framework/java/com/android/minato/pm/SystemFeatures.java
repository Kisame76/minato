package com.android.minato.pm;

import java.util.Collections;
import java.util.HashMap;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;

/**
 * The system features PackageManager reports (ADR-0045 decision 4, ADR-0039 b3.5b1): hasSystemFeature,
 * getSystemAvailableFeatures and a manifest's required features. Android's come from /system/etc/permissions; Minato's
 * are this table, checked against Android 15's reference AVD (apk-runner-35-bare, whose image declares the same
 * features as apk-runner-35: fixtures/expected/system-features.txt) by SystemFeaturesTest, every difference with its
 * reason. Vulkan's features stand only while the session has a Vulkan driver, as Android's stand only with one; the
 * OpenGL ES version is no feature here: getSystemAvailableFeatures adds it from ro.opengles.version, which follows
 * the session's backend (ADR-0044). Plain Java, so tools/framework-host-check.sh runs it on the host's JVM.
 */
public final class SystemFeatures {
    /** Each feature with its version, 0 for none: what backs it on the iPhone is in the comment. */
    public static final Map<String, Integer> FEATURES;
    /** The reference's features Minato does not report, each with the reason. */
    public static final Map<String, String> LEFT_OUT;
    /** Minato's features the reference does not have, or has with another version, each with the reason. */
    public static final Map<String, String> DEPARTURES;
    /** The features that need the session's Vulkan driver (MoltenVK). */
    public static final Set<String> VULKAN = Set.of("android.hardware.vulkan.compute",
            "android.hardware.vulkan.level", "android.hardware.vulkan.version");

    static {
        Map<String, Integer> features = new TreeMap<>();
        // AudioUnit's output (ADR-0036).
        features.put("android.hardware.audio.output", 0);
        // Implied by any touchscreen (PackageManager.FEATURE_FAKETOUCH).
        features.put("android.hardware.faketouch", 0);
        // isLowRamDevice() is false: the device's memory (ADR-0045 decision 4).
        features.put("android.hardware.ram.normal", 0);
        // Sessions turn (ADR-0038).
        features.put("android.hardware.screen.landscape", 0);
        features.put("android.hardware.screen.portrait", 0);
        // The iPhone's touch: five and more independent pointers through MotionEvent and AInputQueue (ADR-0028).
        features.put("android.hardware.touchscreen", 0);
        features.put("android.hardware.touchscreen.multitouch", 0);
        features.put("android.hardware.touchscreen.multitouch.distinct", 0);
        features.put("android.hardware.touchscreen.multitouch.jazzhand", 0);
        // MoltenVK: core Vulkan's compute, and Vulkan 1.3.0 as libvulkan's vkEnumerateInstanceVersion answers.
        features.put("android.hardware.vulkan.compute", 0);
        features.put("android.hardware.vulkan.level", 0);
        features.put("android.hardware.vulkan.version", 0x403000);
        // Android 11's package visibility holds: the app sees no other package (ADR-0039 decision 7).
        features.put("android.software.app_enumeration", 0);
        FEATURES = Collections.unmodifiableMap(features);

        Map<String, String> departures = new TreeMap<>();
        departures.put("android.hardware.vulkan.level", "level 0 until graphics.probe shows the CDD's level-1 features"
                + " on the iPhone: an app that finds less takes a lower path, one that finds more can crash");
        DEPARTURES = Collections.unmodifiableMap(departures);

        Map<String, String> left = new TreeMap<>();
        String camera = "no camera (ADR-0045 decision 4)";
        for (String name : new String[] {"camera", "camera.any", "camera.autofocus",
                "camera.capability.manual_post_processing", "camera.capability.manual_sensor", "camera.capability.raw",
                "camera.concurrent", "camera.flash", "camera.front", "camera.level.full"}) {
            left.put("android.hardware." + name, camera);
        }
        String sensors = "no sensors (ADR-0045 decision 4)";
        for (String name : new String[] {"accelerometer", "ambient_temperature", "barometer", "compass", "gyroscope",
                "light", "proximity", "relative_humidity"}) {
            left.put("android.hardware.sensor." + name, sensors);
        }
        String telephony = "no telephony: Minato answers as a device without it (ADR-0045 decision 4)";
        for (String name : new String[] {"telephony", "telephony.calling", "telephony.data", "telephony.gsm",
                "telephony.ims", "telephony.messaging", "telephony.radio.access", "telephony.subscription"}) {
            left.put("android.hardware." + name, telephony);
        }
        left.put("android.software.telecom", telephony);
        String location = "no location (ADR-0045 decision 4)";
        for (String name : new String[] {"location", "location.gps", "location.network"}) {
            left.put("android.hardware." + name, location);
        }
        String network = "no network: ConnectivityManager and WifiManager report none (ADR-0045 decision 4)";
        for (String name : new String[] {"wifi", "wifi.direct", "wifi.passpoint"}) {
            left.put("android.hardware." + name, network);
        }
        left.put("android.software.ipsec_tunnels", network);
        left.put("android.software.ipsec_tunnel_migration", network);
        String bluetooth = "no Bluetooth (ADR-0045 decision 4)";
        left.put("android.hardware.bluetooth", bluetooth);
        left.put("android.hardware.bluetooth_le", bluetooth);
        left.put("android.hardware.microphone",
                "no audio input in M4, and RECORD_AUDIO is denied (ADR-0045 decision 6)");
        String biometrics = "Face ID is not bridged to the guest";
        left.put("android.hardware.biometrics.face", biometrics);
        left.put("android.hardware.fingerprint", biometrics);
        String keystore = "no Android keystore or identity HAL, so no hardware-backed key security";
        for (String name : new String[] {"hardware_keystore", "identity_credential", "keystore.app_attest_key",
                "security.model.compatible", "reboot_escrow"}) {
            left.put("android.hardware." + name, keystore);
        }
        String deqp = "ANGLE and MoltenVK have not passed Android's dEQP";
        left.put("android.software.opengles.deqp.level", deqp);
        left.put("android.software.vulkan.deqp.level", deqp);
        String user = "one user";
        left.put("android.software.managed_users", user);
        left.put("android.software.securely_removes_users", user);
        left.put("android.software.activities_on_secondary_displays", "one display");
        left.put("android.software.adoptable_storage", "no SD card");
        left.put("android.software.app_widgets", "no widget host");
        left.put("android.software.autofill", "no autofill service");
        left.put("android.software.backup", "no backup transport");
        left.put("android.software.companion_device_setup", "no companion devices");
        left.put("android.software.controls", "no device controls");
        left.put("android.software.credentials", "no credential providers");
        left.put("android.software.device_admin", "no device policy");
        left.put("android.software.device_lock", "no device lock service");
        left.put("android.software.home_screen", "Minato is not the guest's launcher");
        left.put("android.software.incremental_delivery", "no incremental installs");
        left.put("android.software.input_methods", "no input methods of other apps");
        left.put("android.software.live_wallpaper", "no wallpapers");
        left.put("android.software.midi", "no MIDI");
        left.put("android.software.picture_in_picture", "no picture in picture");
        left.put("android.software.print", "no print service");
        left.put("android.software.sdksandbox.sdk_install_work_profile", "no SDK sandbox or work profile");
        left.put("android.software.voice_recognizers", "no speech recognition service");
        left.put("android.software.webview", "no WebView in M4");
        left.put("android.software.window_magnification", "no magnifier");
        left.put("android.software.cts", "not a device Android's compatibility test suite passed");
        left.put("android.software.verified_boot", "not Android's verified boot");
        left.put("android.software.erofs", "no Android kernel");
        left.put("android.software.file_based_encryption", "no credential- and device-encrypted storage or direct"
                + " boot; iOS's data protection encrypts underneath");
        left.put("android.software.secure_lock_screen", "no Android keyguard; ADR-0045's keyguard is unlocked");
        left.put("android.software.cant_save_state",
                "iOS may end a backgrounded app at any time, so no app can keep a state it cannot save");
        LEFT_OUT = Collections.unmodifiableMap(left);
    }

    private SystemFeatures() {
    }

    /** The features as PackageManager reports them, Vulkan's only with a Vulkan driver in the session. */
    public static Map<String, Integer> available(boolean vulkan) {
        if (vulkan) {
            return FEATURES;
        }
        Map<String, Integer> features = new HashMap<>(FEATURES);
        features.keySet().removeAll(VULKAN);
        return Collections.unmodifiableMap(features);
    }

    /** PackageManagerService.hasSystemFeature: the feature, at least in this version. */
    public static boolean has(String name, int version, boolean vulkan) {
        Integer have = available(vulkan).get(name);
        return have != null && have >= version;
    }
}
