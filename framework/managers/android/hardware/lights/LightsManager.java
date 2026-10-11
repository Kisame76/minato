// The written part of LightsManager (the gap list of 2026-10-11: InputDevice's methods name it, and a class whose
// method types cannot be resolved cannot be read by reflection), which tools/managergen joins with the API's methods:
// the hidden constructor that an input device's lights manager calls. Before, the hidden member of a compile-only stub
// (ADR-0045, the input service).
class LightsManager {
    /** @hide */
    public LightsManager() {
    }
}
