package io.github.kisame76.minato.test.answers;

import android.app.ActivityManager;
import android.app.Application;
import android.app.AppOpsManager;
import android.content.Context;
import android.media.AudioDeviceCallback;
import android.media.AudioDeviceInfo;
import android.media.AudioManager;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import android.os.Handler;
import android.os.Looper;
import android.telephony.TelephonyManager;
import android.util.Log;

import java.util.concurrent.Callable;

/**
 * Manager methods that Minato answered with a default and a log line (the compatibility plan's decision 6): each call
 * is logged with its result, or with the class of the exception it throws. What differs by device (a size in pixels, a
 * network's name) is tagged "-device" and only shown.
 */
public class AnswersApplication extends Application {
    private static final String TAG = "manager-answers-test";

    private static void log(String name, Callable<Object> call) {
        String result;
        try {
            result = String.valueOf(call.call());
        } catch (Throwable e) {
            result = "throws " + e.getClass().getName();
        }
        Log.i(TAG, name + ": " + result);
    }

    @Override
    public void onCreate() {
        super.onCreate();
        new Handler(Looper.getMainLooper()).post(this::run);
    }

    private void run() {
        final ActivityManager activities = getSystemService(ActivityManager.class);
        final int size = activities.getLauncherLargeIconSize();
        final float density = getResources().getDisplayMetrics().density;
        Log.i(TAG, "ActivityManager.getLauncherLargeIconSize in dp: " + Math.round(size / density));
        Log.i(TAG + "-device", "ActivityManager.getLauncherLargeIconSize in px: " + size);

        final AppOpsManager appOps = getSystemService(AppOpsManager.class);
        log("AppOpsManager.permissionToOp(CAMERA)", () -> AppOpsManager.permissionToOp("android.permission.CAMERA"));
        log("AppOpsManager.permissionToOp(INTERNET)",
                () -> AppOpsManager.permissionToOp("android.permission.INTERNET"));
        log("AppOpsManager.permissionToOp(unknown)", () -> AppOpsManager.permissionToOp("not.a.permission"));
        log("AppOpsManager.noteProxyOp", () -> appOps.noteProxyOp(AppOpsManager.OPSTR_CAMERA, getPackageName()));
        log("AppOpsManager.noteProxyOpNoThrow",
                () -> appOps.noteProxyOpNoThrow(AppOpsManager.OPSTR_CAMERA, getPackageName()));

        final TelephonyManager telephony = getSystemService(TelephonyManager.class);
        log("TelephonyManager.getDeviceId(0)", () -> telephony.getDeviceId(0));
        try {
            Log.i(TAG + "-device", "TelephonyManager.getSubscriptionId: " + telephony.getSubscriptionId());
        } catch (Throwable e) {
            Log.i(TAG + "-device", "TelephonyManager.getSubscriptionId throws " + e.getClass().getName());
        }

        final WifiManager wifi = getSystemService(WifiManager.class);
        log("WifiManager.getConnectionInfo is not null", () -> wifi.getConnectionInfo() != null);
        log("WifiManager.getConnectionInfo ssid without location", () -> wifi.getConnectionInfo().getSSID());
        log("WifiManager.getConnectionInfo bssid without location", () -> wifi.getConnectionInfo().getBSSID());
        log("WifiManager.getConnectionInfo mac", () -> wifi.getConnectionInfo().getMacAddress());
        log("WifiManager.getConnectionInfo network id < 0 or not", () -> wifi.getConnectionInfo().getNetworkId() <= 0);
        log("WifiManager.getConnectionInfo supplicant state", () -> wifi.getConnectionInfo().getSupplicantState());

        final AudioManager audio = getSystemService(AudioManager.class);
        log("AudioManager.playSoundEffect", () -> {
            audio.playSoundEffect(AudioManager.FX_KEY_CLICK);
            return "returned";
        });
        final int[] added = new int[1];
        final AudioDeviceCallback callback = new AudioDeviceCallback() {
            @Override
            public void onAudioDevicesAdded(AudioDeviceInfo[] devices) {
                added[0] += devices.length;
            }
        };
        log("AudioManager.registerAudioDeviceCallback", () -> {
            audio.registerAudioDeviceCallback(callback, new Handler(Looper.getMainLooper()));
            return "returned";
        });
        new Handler(Looper.getMainLooper()).postDelayed(() -> {
            Log.i(TAG, "AudioManager.registerAudioDeviceCallback heard the devices that are there: " + (added[0] > 0));
            log("AudioManager.unregisterAudioDeviceCallback", () -> {
                audio.unregisterAudioDeviceCallback(callback);
                return "returned";
            });
            Log.i(TAG, "done");
        }, 1500);
    }
}
