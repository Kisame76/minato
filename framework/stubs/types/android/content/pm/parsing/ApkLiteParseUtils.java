package android.content.pm.parsing;

import android.content.pm.parsing.result.ParseInput;
import android.content.pm.parsing.result.ParseResult;
import java.io.File;
import java.io.FileDescriptor;

/** Compile-only stub (the Unity test APK resolve list, batch E), never in framework.jar: the package parser PackageInstaller.readInstallInfo calls. */
public class ApkLiteParseUtils {
    public static ParseResult<PackageLite> parsePackageLite(ParseInput input, File packageFile, int flags) {
        return null;
    }

    public static ParseResult<PackageLite> parseMonolithicPackageLite(ParseInput input, FileDescriptor packageFd,
            String debugPathName, int flags) {
        return null;
    }
}
