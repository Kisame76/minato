// The written part of AudioDeviceCallback: the abstract class an app extends to hear audio devices come and go, whose
// callbacks do nothing until it overrides them, as AOSP's.
class AudioDeviceCallback {
    public AudioDeviceCallback() {
    }

    public void onAudioDevicesAdded(AudioDeviceInfo[] addedDevices) {
    }

    public void onAudioDevicesRemoved(AudioDeviceInfo[] removedDevices) {
    }
}
