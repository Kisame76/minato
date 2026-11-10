// The written part of MediaCommunicationManager (service inventory, bundle B rest): no media session of another app is
// visible to a guest, and the version is Android 15's [verified: services-h-test].
import android.content.Context;
import java.util.ArrayList;
import java.util.List;

class MediaCommunicationManager {
    private final Context mContext;

    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public MediaCommunicationManager(Context context) {
        mContext = context;
    }

    public List<Session2Token> getSession2Tokens() {
        return new ArrayList<>();
    }

    public int getVersion() {
        return 1;
    }
}
