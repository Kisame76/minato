// The written part of BlobStoreManager (service inventory, bundle B): no blob is leased, and the quota left is Android 15's
// 300 MiB on the emulator without the phone hardware [verified: services-h-test].
import android.content.Context;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

class BlobStoreManager {
    private final Context mContext;

    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public BlobStoreManager(Context context) {
        mContext = context;
    }

    public List<BlobHandle> getLeasedBlobs() throws IOException {
        return new ArrayList<>();
    }

    public long getRemainingLeaseQuotaBytes() {
        return 314572800L;
    }
}
