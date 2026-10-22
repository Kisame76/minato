// The written part of ServiceState (F4d): the hidden boost that the cell signal strengths add to a reading on an ARFCN
// the carrier lists. Minato has no radio, so no service state carries one: AOSP's field is 0 until the telephony
// service sets it [verified: ServiceState.java at android-15.0.0_r1, frameworks/base/telephony/java/android/telephony].
class ServiceState {
    /** @hide */
    public int getArfcnRsrpBoost() {
        return 0;
    }
}
