package android.os;

/**
 * Compile-only stub (ADR-0039 step c2c1), never in framework.jar: the asynchronous getType and canonicalize answers
 * of ContentProvider's transport, to another process.
 */
public final class RemoteCallback implements android.os.Parcelable {
    public interface OnResultListener {
    }

    public RemoteCallback(OnResultListener listener) {
        throw new RuntimeException("stub");
    }

    public RemoteCallback( OnResultListener listener, Handler handler) {
        throw new RuntimeException("stub");
    }

    RemoteCallback(Parcel parcel) {
        throw new RuntimeException("stub");
    }

    public void sendResult( final Bundle result) {
        throw new RuntimeException("stub");
    }

    public static Parcelable.Creator<RemoteCallback> CREATOR;

    public void writeToParcel(Parcel parcel, int flags) {
        throw new RuntimeException("stub");
    }

    public int describeContents() {
        throw new RuntimeException("stub");
    }
}
