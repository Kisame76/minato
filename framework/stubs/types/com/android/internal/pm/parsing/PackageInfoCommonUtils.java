package com.android.internal.pm.parsing;

import android.content.pm.PackageInfo;
import com.android.server.pm.pkg.AndroidPackage;

/** Compile-only stub (ADR-0039 b3.5a), never in framework.jar: getPackageArchiveInfo's PackageInfo comes from it. */
public class PackageInfoCommonUtils {
    public static PackageInfo generate(AndroidPackage pkg, long flags, int userId) {
        throw new RuntimeException("stub");
    }
}
