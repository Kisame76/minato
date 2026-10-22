package android.os;

/**
 * Minato's android.os.IHwBinder (ADR-0060 e2b3): the HIDL binder's type, which MediaCodec's configure names for a
 * descrambler's binder, without HIDL's transact and death recipients: Minato has no HIDL services, so no binder of
 * this type is ever made and MediaCodec passes null.
 * @hide
 */
public interface IHwBinder {
}
