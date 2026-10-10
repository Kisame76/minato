package com.android.internal.content;

import android.content.pm.parsing.PackageLite;
import java.io.FileDescriptor;
import java.io.IOException;

/** Compile-only stub (the Unity test APK resolve list, batch E), never in framework.jar: the installed size PackageInstaller.InstallInfo asks for. */
public class InstallLocationUtils {
    public static long calculateInstalledSize(PackageLite pkg, String abiOverride) throws IOException {
        return 0;
    }

    public static long calculateInstalledSize(PackageLite pkg, String abiOverride, FileDescriptor fd)
            throws IOException {
        return 0;
    }
}
