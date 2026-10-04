package com.android.sdksandbox.flags;

/**
 * Compile-only stub (ADR-0039 step c2c2), never in framework.jar: Activity.attach asks the SDK sandbox module's flag
 * sandboxActivitySdkBasedContext, whose aconfig declarations tools/aconfig-flags.py does not read; a real class must
 * answer it at run time.
 */
public final class Flags {
}
