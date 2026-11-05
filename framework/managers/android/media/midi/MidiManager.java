// The written part of MidiManager (services batch B): no MIDI device is connected, as on the emulator (apk-runner-35)
// [verified: services-b-test].
import android.content.Context;

class MidiManager {
    /** @hide As the tag's, which SystemServiceRegistry calls with the outer context. */
    public MidiManager(Context context) {
    }

    public MidiDeviceInfo[] getDevices() {
        return new MidiDeviceInfo[0];
    }

    public java.util.Set<MidiDeviceInfo> getDevicesForTransport(int transport) {
        return new java.util.HashSet<>();
    }
}
