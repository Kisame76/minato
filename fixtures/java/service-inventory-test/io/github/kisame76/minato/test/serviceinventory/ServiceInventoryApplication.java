package io.github.kisame76.minato.test.serviceinventory;

import android.app.Application;
import android.util.Log;

/**
 * Which of Android 15's public system services an app gets from getSystemService(name): one line per public
 * Context.*_SERVICE constant (API 35), "service CONSTANT: <class of the object>" or "null". The list is the constants
 * of Context in the public API file, as the platform lists them. An exception shows as its class name.
 */
public class ServiceInventoryApplication extends Application {
    private static final String TAG = "service-inventory-test";

    private static final String[][] SERVICES = {
        {"ACCESSIBILITY_SERVICE", "accessibility"},
        {"ACCOUNT_SERVICE", "account"},
        {"ACTIVITY_SERVICE", "activity"},
        {"ALARM_SERVICE", "alarm"},
        {"APPWIDGET_SERVICE", "appwidget"},
        {"APP_OPS_SERVICE", "appops"},
        {"APP_SEARCH_SERVICE", "app_search"},
        {"AUDIO_SERVICE", "audio"},
        {"BATTERY_SERVICE", "batterymanager"},
        {"BIOMETRIC_SERVICE", "biometric"},
        {"BLOB_STORE_SERVICE", "blob_store"},
        {"BLUETOOTH_SERVICE", "bluetooth"},
        {"BUGREPORT_SERVICE", "bugreport"},
        {"CAMERA_SERVICE", "camera"},
        {"CAPTIONING_SERVICE", "captioning"},
        {"CARRIER_CONFIG_SERVICE", "carrier_config"},
        {"CLIPBOARD_SERVICE", "clipboard"},
        {"COMPANION_DEVICE_SERVICE", "companiondevice"},
        {"CONNECTIVITY_DIAGNOSTICS_SERVICE", "connectivity_diagnostics"},
        {"CONNECTIVITY_SERVICE", "connectivity"},
        {"CONSUMER_IR_SERVICE", "consumer_ir"},
        {"CREDENTIAL_SERVICE", "credential"},
        {"CROSS_PROFILE_APPS_SERVICE", "crossprofileapps"},
        {"DEVICE_LOCK_SERVICE", "device_lock"},
        {"DEVICE_POLICY_SERVICE", "device_policy"},
        {"DISPLAY_HASH_SERVICE", "display_hash"},
        {"DISPLAY_SERVICE", "display"},
        {"DOMAIN_VERIFICATION_SERVICE", "domain_verification"},
        {"DOWNLOAD_SERVICE", "download"},
        {"DROPBOX_SERVICE", "dropbox"},
        {"EUICC_SERVICE", "euicc"},
        {"FILE_INTEGRITY_SERVICE", "file_integrity"},
        {"FINGERPRINT_SERVICE", "fingerprint"},
        {"GAME_SERVICE", "game"},
        {"GRAMMATICAL_INFLECTION_SERVICE", "grammatical_inflection"},
        {"HARDWARE_PROPERTIES_SERVICE", "hardware_properties"},
        {"HEALTHCONNECT_SERVICE", "healthconnect"},
        {"INPUT_METHOD_SERVICE", "input_method"},
        {"INPUT_SERVICE", "input"},
        {"IPSEC_SERVICE", "ipsec"},
        {"JOB_SCHEDULER_SERVICE", "jobscheduler"},
        {"KEYGUARD_SERVICE", "keyguard"},
        {"LAUNCHER_APPS_SERVICE", "launcherapps"},
        {"LOCALE_SERVICE", "locale"},
        {"LOCATION_SERVICE", "location"},
        {"MEDIA_COMMUNICATION_SERVICE", "media_communication"},
        {"MEDIA_METRICS_SERVICE", "media_metrics"},
        {"MEDIA_PROJECTION_SERVICE", "media_projection"},
        {"MEDIA_ROUTER_SERVICE", "media_router"},
        {"MEDIA_SESSION_SERVICE", "media_session"},
        {"MIDI_SERVICE", "midi"},
        {"NETWORK_STATS_SERVICE", "netstats"},
        {"NFC_SERVICE", "nfc"},
        {"NOTIFICATION_SERVICE", "notification"},
        {"NSD_SERVICE", "servicediscovery"},
        {"OVERLAY_SERVICE", "overlay"},
        {"PEOPLE_SERVICE", "people"},
        {"PERFORMANCE_HINT_SERVICE", "performance_hint"},
        {"POWER_SERVICE", "power"},
        {"PRINT_SERVICE", "print"},
        {"RESTRICTIONS_SERVICE", "restrictions"},
        {"ROLE_SERVICE", "role"},
        {"SEARCH_SERVICE", "search"},
        {"SENSOR_SERVICE", "sensor"},
        {"SHORTCUT_SERVICE", "shortcut"},
        {"STATUS_BAR_SERVICE", "statusbar"},
        {"STORAGE_SERVICE", "storage"},
        {"STORAGE_STATS_SERVICE", "storagestats"},
        {"SYSTEM_HEALTH_SERVICE", "systemhealth"},
        {"TELECOM_SERVICE", "telecom"},
        {"TELEPHONY_IMS_SERVICE", "telephony_ims"},
        {"TELEPHONY_SERVICE", "phone"},
        {"TELEPHONY_SUBSCRIPTION_SERVICE", "telephony_subscription_service"},
        {"TEXT_CLASSIFICATION_SERVICE", "textclassification"},
        {"TEXT_SERVICES_MANAGER_SERVICE", "textservices"},
        {"TV_INPUT_SERVICE", "tv_input"},
        {"TV_INTERACTIVE_APP_SERVICE", "tv_interactive_app"},
        {"UI_MODE_SERVICE", "uimode"},
        {"USAGE_STATS_SERVICE", "usagestats"},
        {"USB_SERVICE", "usb"},
        {"USER_SERVICE", "user"},
        {"VIBRATOR_MANAGER_SERVICE", "vibrator_manager"},
        {"VIRTUAL_DEVICE_SERVICE", "virtualdevice"},
        {"VPN_MANAGEMENT_SERVICE", "vpn_management"},
        {"WIFI_AWARE_SERVICE", "wifiaware"},
        {"WIFI_P2P_SERVICE", "wifip2p"},
        {"WIFI_RTT_RANGING_SERVICE", "wifirtt"},
        {"WIFI_SERVICE", "wifi"}
    };

    @Override
    public void onCreate() {
        super.onCreate();
        for (final String[] service : SERVICES) {
            String answer;
            try {
                final Object object = getSystemService(service[1]);
                answer = object == null ? "null" : object.getClass().getName();
            } catch (Throwable t) {
                answer = "throws " + t.getClass().getName();
            }
            Log.i(TAG, "service " + service[0] + ": " + answer);
        }
        Log.i(TAG, "done");
    }
}
