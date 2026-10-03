package com.android.internal.pm.parsing;

import android.content.pm.ApplicationInfo;
import android.util.DisplayMetrics;
import com.android.internal.pm.parsing.pkg.ParsedPackage;
import com.android.internal.pm.pkg.parsing.ParsingPackageUtils;
import java.io.File;

/**
 * Compile-only stub (ADR-0039 b3.5a), never in framework.jar: getPackageArchiveInfo's parser, whose Callback an
 * anonymous class of PackageManager extends; framework/stub-supertypes.txt lists that class.
 */
public class PackageParser2 {
    public PackageParser2(String[] separateProcesses, DisplayMetrics displayMetrics, IPackageCacher cacher,
            Callback callback) {
        throw new RuntimeException("stub");
    }

    public ParsedPackage parsePackage(File packageFile, int flags, boolean useCaches) throws PackageParserException {
        throw new RuntimeException("stub");
    }

    public abstract static class Callback implements ParsingPackageUtils.Callback {
        public abstract boolean isChangeEnabled(long changeId, ApplicationInfo appInfo);
    }
}
