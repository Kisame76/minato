// The written part of LauncherActivityInfo for tools/managers-check.sh only (tools/build-framework.sh compiles AOSP's
// own class): the hidden constructor LauncherApps makes its entries with, which the check's generated class lacks.
import android.content.Context;

class LauncherActivityInfo {
    /** @hide */
    public LauncherActivityInfo(Context context, LauncherActivityInfoInternal internal) {
    }
}
