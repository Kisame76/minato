// The written part of StatusBarManager (services bundle A of the system services gap): the status bar's services
// (collapsing the panels, tiles). An app holds no STATUS_BAR permission and Minato has no status bar of its own, so the
// manager is there and every API call is the generator's unwritten answer. getSystemService hands it out, as the
// emulator (apk-runner-35-bare) does [verified: services-f-test].
import android.content.Context;

class StatusBarManager {
    /** @hide As the tag's, which SystemServiceRegistry calls. */
    public StatusBarManager(Context context) {
    }
}
