package android.os;

/**
 * Compile-only stub (ADR-0039 b3.5a), never in framework.jar: BaseBundle catches it when a parcelled value has another
 * type, which Parcel throws once Parcel's natives come.
 */
public class BadTypeParcelableException extends BadParcelableException {
    public BadTypeParcelableException(String msg) {
        super(msg);
        throw new RuntimeException("stub");
    }
}
