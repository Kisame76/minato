// The written part of OverlayInfo (service inventory, bundle B): the hidden members ResourcesProvider calls. Minato has no
// overlays, so an OverlayInfo an app holds is not fabricated and has no code path.
class OverlayInfo {
    /** @hide */
    public String getBaseCodePath() {
        return null;
    }

    /** @hide */
    public boolean isFabricated() {
        return false;
    }

    /** @hide */
    public String getTargetOverlayableName() {
        return null;
    }

    /** @hide */
    public String getOverlayName() {
        return null;
    }
}
