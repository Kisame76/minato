// The written part of MediaDescrambler (ADR-0060 e2b3), which tools/managergen joins with the API's methods: the hidden
// getBinder that AOSP's MediaCodec.configure asks of a descrambler. Minato has no CAS or descrambler service, so no
// descrambler has a binder; MediaCodec passes null on, as for no descrambler.
import android.os.IHwBinder;

class MediaDescrambler {
    /** @hide */
    IHwBinder getBinder() {
        return null;
    }
}
