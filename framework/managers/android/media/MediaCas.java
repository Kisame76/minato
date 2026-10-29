// The written part of MediaCas (ADR-0060 e2d part 2b), which tools/managergen joins with the API's methods: the hidden
// getBinder and createFromSessionId that AOSP's MediaExtractor.setMediaCas and getCasInfo ask of a CAS. Minato has no
// CAS service, so no CAS has a binder (MediaExtractor's natives refuse it as Android's refuse a null one) and no
// session.
import android.os.IHwBinder;

class MediaCas {
    /** @hide */
    IHwBinder getBinder() {
        return null;
    }

    /** @hide */
    Session createFromSessionId(byte[] sessionId) {
        return null;
    }
}
