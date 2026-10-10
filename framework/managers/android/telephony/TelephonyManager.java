// The written part of TelephonyManager (ADR-0045 decision 4; the Unity test APK probe: Unity's launcher activity listens for the
// call state and Firebase's datatransport reads the SIM's operator), which tools/managergen joins with the API's
// methods and constants. Minato answers as a phone without a SIM and without a telephony service, as ADR-0045 decision 4
// has it for the features (no android.hardware.telephony): the iPhone has a modem, but no phone data reaches an app.
// The answers are TelephonyManager's own for getITelephony() == null, on a device whose config_voice_capable and
// config_sms_capable are false and whose telephony properties are unset [verified: TelephonyManager.java at
// android-15.0.0_r1, frameworks/base/telephony/java/android/telephony]: no service means CALL_STATE_IDLE
// (getCallState reaches TelecomManager, whose failure also gives idle), NETWORK_TYPE_UNKNOWN, DATA_DISCONNECTED,
// DATA_ACTIVITY_NONE, no ICC card, empty operator, country and name strings from the unset properties, no roaming, and
// null for the device and subscriber identifiers; getPhoneType is PHONE_TYPE_NONE while the device is not voice capable;
// getSimState is SIM_STATE_ABSENT as getSimStateIncludingLoaded has it when no slot reports a SIM. listen() and
// registerTelephonyCallback() take the listener and, as the tag's notifyNow is (getITelephony() != null), call it back
// with nothing: the registry gives the first state only when the phone service exists. What the registry's permission
// check would refuse an app that targets Android 12 or later and lacks READ_PHONE_STATE is not refused here. Every
// other method is the generated one, logged once with its default.
import android.content.Context;
import android.telephony.PhoneStateListener;
import android.telephony.TelephonyCallback;
import java.util.concurrent.Executor;

class TelephonyManager {
    private final Context mContext;

    /**
     * @hide SystemServiceRegistry's, with the context it builds the manager for. Android's takes the context (the
     * public one is deprecated) and finds its subscription from it.
     */
    public TelephonyManager(Context context) {
        mContext = context;
    }

    public TelephonyManager createForSubscriptionId(int subId) {
        return new TelephonyManager(mContext);
    }

    /**
     * TelecomManager.getCallState's: an app that targets Android 12 or later needs READ_PHONE_STATE, else the
     * SecurityException of Context.enforceCallingOrSelfPermission [verified: telephony-test on apk-runner-35].
     */
    public int getCallState() {
        if (mContext != null && targetsS()) {
            mContext.enforceCallingOrSelfPermission("android.permission.READ_PHONE_STATE", "getCallState");
        }
        return CALL_STATE_IDLE;
    }

    public int getCallStateForSubscription() {
        return CALL_STATE_IDLE;
    }

    public int getNetworkType() {
        return NETWORK_TYPE_UNKNOWN;
    }

    public int getDataNetworkType() {
        return NETWORK_TYPE_UNKNOWN;
    }

    public int getVoiceNetworkType() {
        return NETWORK_TYPE_UNKNOWN;
    }

    public int getPhoneType() {
        return PHONE_TYPE_NONE;
    }

    public int getSimState() {
        return SIM_STATE_ABSENT;
    }

    public boolean hasIccCard() {
        return false;
    }

    public String getSimOperator() {
        return "";
    }

    public String getSimOperatorName() {
        return "";
    }

    public String getSimCountryIso() {
        return "";
    }

    public String getNetworkOperator() {
        return "";
    }

    public String getNetworkOperatorName() {
        return "";
    }

    public String getNetworkCountryIso() {
        return "";
    }

    public boolean isNetworkRoaming() {
        return false;
    }

    public int getDataState() {
        return DATA_DISCONNECTED;
    }

    public int getDataActivity() {
        return DATA_ACTIVITY_NONE;
    }

    public boolean isDataEnabled() {
        return false;
    }

    public boolean isVoiceCapable() {
        return false;
    }

    public boolean isSmsCapable() {
        return false;
    }

    public String getDeviceId() {
        return null;
    }

    public String getImei() {
        return null;
    }

    public String getSubscriberId() {
        return null;
    }

    public String getLine1Number() {
        return null;
    }

    public void listen(PhoneStateListener listener, int events) {
        checkCallStateListener((events & PhoneStateListener.LISTEN_CALL_STATE) != 0);
    }

    public void registerTelephonyCallback(Executor executor, TelephonyCallback callback) {
        if (mContext == null) {
            throw new IllegalStateException("telephony service is null.");
        }
        if (executor == null || callback == null) {
            throw new IllegalArgumentException("TelephonyCallback and executor must be non-null");
        }
        checkCallStateListener(callback instanceof TelephonyCallback.CallStateListener);
    }

    /**
     * The registry's check for a call state listener: an app that targets Android 12 or later needs READ_PHONE_STATE,
     * else a SecurityException whose message is the method's name [verified: telephony-test on apk-runner-35].
     */
    private void checkCallStateListener(boolean callState) {
        if (callState && mContext != null && targetsS()
                && mContext.checkSelfPermission("android.permission.READ_PHONE_STATE")
                        != android.content.pm.PackageManager.PERMISSION_GRANTED) {
            throw new SecurityException("listen");
        }
    }

    private boolean targetsS() {
        // Build.VERSION_CODES.S, written out: tools/managers-check.sh compiles this class without the annotations Build names.
        return mContext.getApplicationInfo().targetSdkVersion >= 31;
    }

    public void unregisterTelephonyCallback(TelephonyCallback callback) {
    }
}
