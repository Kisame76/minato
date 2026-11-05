// The written part of NfcManager (services batch B): the manager is there and has no adapter, as on the emulator
// (apk-runner-35), which has no NFC; an iPhone's NFC reader is not offered to guests [verified: services-b-test].
import android.content.Context;

class NfcManager {
    private final Context mContext;

    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public NfcManager(Context context) {
        mContext = context;
    }

    public NfcAdapter getDefaultAdapter() {
        return null;
    }
}
