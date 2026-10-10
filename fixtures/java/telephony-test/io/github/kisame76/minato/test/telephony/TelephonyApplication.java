package io.github.kisame76.minato.test.telephony;

import android.app.Application;
import android.content.Context;
import android.content.pm.PackageManager;
import android.telephony.PhoneStateListener;
import android.telephony.TelephonyCallback;
import android.telephony.TelephonyManager;
import android.util.Log;

/**
 * The telephony manager as Unity's launcher activity and Firebase's datatransport use it (the Unity test APK probe): the
 * service, a PhoneStateListener, listening and the telephony callbacks, and the arguments they refuse. The lines of
 * telephony-test are Android's on any device; the lines of telephony-test-device (the SIM, the radio, the operator,
 * the features) depend on the device and are only shown.
 */
public class TelephonyApplication extends Application {
    static final String TAG = "telephony-test";
    static final String DEVICE = "telephony-test-device";

    private static void log(String line) {
        Log.i(TAG, line);
    }

    private static void device(String line) {
        Log.i(DEVICE, line);
    }

    private interface Call {
        Object run() throws Exception;
    }

    private static String outcome(Call call) {
        try {
            return String.valueOf(call.run());
        } catch (Exception e) {
            // The app's uid differs at every install.
            return e.getClass().getName() + ": " + String.valueOf(e.getMessage()).replaceAll("\\b1[0-9]{4}\\b", "UID");
        }
    }

    private static final class Idle extends TelephonyCallback implements TelephonyCallback.CallStateListener {
        @Override
        public void onCallStateChanged(int state) {
        }
    }

    @Override
    public void onCreate() {
        super.onCreate();
        TelephonyManager telephony = (TelephonyManager) getSystemService(Context.TELEPHONY_SERVICE);
        log("the service: " + (telephony == null ? "null" : telephony.getClass().getName()));
        log("the same service again: " + (getSystemService(Context.TELEPHONY_SERVICE) == telephony));
        log("by class: " + (getSystemService(TelephonyManager.class) == telephony));
        log("the call state: " + outcome(telephony::getCallState));
        PhoneStateListener listener = new PhoneStateListener() {
            @Override
            public void onCallStateChanged(int state, String phoneNumber) {
                device("the listener was called: " + state + " [" + phoneNumber + "]");
            }
        };
        log("a listener: " + (listener != null));
        telephony.listen(listener, PhoneStateListener.LISTEN_NONE);
        log("listen to nothing: fine");
        log("a null executor: " + outcome(() -> {
            telephony.registerTelephonyCallback(null, new Idle());
            return "fine";
        }));
        log("a null callback: " + outcome(() -> {
            telephony.registerTelephonyCallback(Runnable::run, null);
            return "fine";
        }));
        log("unregister a callback that was never registered: " + outcome(() -> {
            telephony.unregisterTelephonyCallback(new Idle());
            return "fine";
        }));
        deviceLines(telephony, listener);
        log("done");
    }

    private void deviceLines(TelephonyManager telephony, PhoneStateListener listener) {
        PackageManager packageManager = getPackageManager();
        device("features: telephony " + packageManager.hasSystemFeature(PackageManager.FEATURE_TELEPHONY)
                + ", calling " + packageManager.hasSystemFeature(PackageManager.FEATURE_TELEPHONY_CALLING)
                + ", radio access " + packageManager.hasSystemFeature(PackageManager.FEATURE_TELEPHONY_RADIO_ACCESS)
                + ", subscription " + packageManager.hasSystemFeature(PackageManager.FEATURE_TELEPHONY_SUBSCRIPTION));
        device("sim state " + telephony.getSimState() + ", phone type " + telephony.getPhoneType() + ", has icc "
                + telephony.hasIccCard() + ", voice capable " + telephony.isVoiceCapable() + ", sms capable "
                + telephony.isSmsCapable());
        device("operators: sim [" + telephony.getSimOperator() + "] [" + telephony.getSimOperatorName() + "] ["
                + telephony.getSimCountryIso() + "], network [" + telephony.getNetworkOperator() + "] ["
                + telephony.getNetworkOperatorName() + "] [" + telephony.getNetworkCountryIso() + "]");
        device("network type " + outcome(telephony::getNetworkType) + ", data network type "
                + outcome(telephony::getDataNetworkType) + ", data state " + telephony.getDataState()
                + ", data activity " + telephony.getDataActivity() + ", roaming " + telephony.isNetworkRoaming());
        device("identifiers: device " + outcome(telephony::getDeviceId) + ", subscriber "
                + outcome(telephony::getSubscriberId) + ", number " + outcome(telephony::getLine1Number));
        device("listen to the call state: " + outcome(() -> {
            telephony.listen(listener, PhoneStateListener.LISTEN_CALL_STATE);
            return "fine";
        }));
        device("a call state callback: " + outcome(() -> {
            telephony.registerTelephonyCallback(Runnable::run, new Idle());
            return "fine";
        }));
        try {
            Thread.sleep(500);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
        telephony.listen(listener, PhoneStateListener.LISTEN_NONE);
    }
}
