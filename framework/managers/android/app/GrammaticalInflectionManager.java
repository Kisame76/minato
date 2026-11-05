// The written part of GrammaticalInflectionManager (services batch C): the app's grammatical gender, which Android
// keeps in the app's configuration. Minato has no system setting for it: an app is asked for none (not specified) until
// it sets one, which this manager then answers; the system's gender is behind READ_SYSTEM_GRAMMATICAL_GENDER, which
// no app holds, so it is the SecurityException Android 15 gives [verified: services-c-test].
import android.content.Context;

class GrammaticalInflectionManager {
    private final Context mContext;
    private int mRequested;

    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public GrammaticalInflectionManager(Context context) {
        mContext = context;
    }

    public int getApplicationGrammaticalGender() {
        return mRequested;
    }

    public int getSystemGrammaticalGender() {
        throw new SecurityException("getSystemGrammaticalGender: Neither user " + android.os.Process.myUid()
                + " nor current process has android.permission.READ_SYSTEM_GRAMMATICAL_GENDER.");
    }

    public void setRequestedApplicationGrammaticalGender(int grammaticalGender) {
        mRequested = grammaticalGender;
    }

    /** @hide Settings' configuration of a user asks for the system's gender; there is none. */
    public int peekSystemGrammaticalGenderByUserId(int userId) {
        return 0;
    }
}
