// The written part of TelecomManager (services batch B): the phone calls, of which Minato has none. An app without
// READ_PHONE_STATE or READ_PHONE_NUMBERS gets the SecurityException Android 15 gives it for the call state and the
// phone accounts [verified: services-b-test].
import android.content.Context;

class TelecomManager {
    private final Context mContext;

    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public TelecomManager(Context context) {
        mContext = context;
    }

    public boolean isInCall() {
        throw new SecurityException("isInCall: Neither user " + android.os.Process.myUid()
                + " nor current process has android.permission.READ_PHONE_STATE.");
    }

    public java.util.List<PhoneAccountHandle> getCallCapablePhoneAccounts() {
        throw new SecurityException("getCallCapablePhoneAccounts: Neither user " + android.os.Process.myUid()
                + " nor current process has android.permission.READ_PHONE_STATE.");
    }
}
