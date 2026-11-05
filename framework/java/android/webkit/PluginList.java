package android.webkit;

import com.android.minato.services.Unwritten;

/**
 * Placeholder (retest 12): our class under AOSP's hidden name, which only has to load, since WebView's hidden
 * getPluginList names it and apps reflect over WebView; its constructor logs once and throws
 * UnsupportedOperationException. Before, a compile-only stub (the Unity test APK probe's batch C): the plugin list removed
 * from the API long ago, whose plugins need classes framework.jar leaves out.
 */
public class PluginList {
    public PluginList() {
        throw Unwritten.placeholder("android.webkit.PluginList()");
    }
}
