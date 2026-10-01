/*
 * Minato: a compile-time stand-in (ADR-0033 a) for tools/platform-compat's android.compat.annotation.EnabledSince;
 * see UnsupportedAppUsage.java.
 */
package android.compat.annotation;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

@Retention(RetentionPolicy.CLASS)
public @interface EnabledSince {
    int targetSdkVersion();
}
