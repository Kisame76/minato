package com.android.internal.pm.pkg.parsing;

import java.util.Set;

/**
 * Compile-only stub (ADR-0039 b3.5a), never in framework.jar: AOSP's package parser, which PackageManager's
 * getPackageArchiveInfo runs (decision 7 rejects it for the app's own manifest). Its flag is a constant javac
 * inlines, so it has the tag's value.
 */
public class ParsingPackageUtils {
    public static final int PARSE_COLLECT_CERTIFICATES = 1 << 5;

    public interface Callback {
        boolean hasFeature(String feature);

        Set<String> getHiddenApiWhitelistedApps();

        Set<String> getInstallConstraintsAllowlist();
    }
}
