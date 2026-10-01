package android.app;

import android.content.pm.ApplicationInfo;
import android.content.res.Resources;
import android.content.res.ApkAssets;
import android.content.res.ResourcesImpl;

import java.io.IOException;

/** Compile-only stub (ADR-0039 b3.1), never in framework.jar: AssetManager, Resources and ResourcesImpl call it. */
public class ResourcesManager {
    public static ResourcesManager getInstance() {
        throw new RuntimeException("stub");
    }

    public LocaleConfig getLocaleConfig() {
        throw new RuntimeException("stub");
    }

    public void registerAllResourcesReference(Resources resources) {
        throw new RuntimeException("stub");
    }

    public void registerResourcePaths(String uniqueId, ApplicationInfo appInfo) {
        throw new RuntimeException("stub");
    }

    public int updateResourceImplWithRegisteredLibs(ResourcesImpl impl) {
        throw new RuntimeException("stub");
    }

    public ApkAssets loadApkAssets(ApkKey key) throws IOException {
        throw new RuntimeException("stub");
    }

    public static class ApkKey {
        public final String path;
        public final boolean sharedLib;
        public final boolean overlay;

        public ApkKey(String path, boolean sharedLib, boolean overlay) {
            this.path = path;
            this.sharedLib = sharedLib;
            this.overlay = overlay;
        }
    }
}
