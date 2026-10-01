/*
 * Minato: a compile-time stand-in (ADR-0033 a) for tools/platform-compat's
 * android.compat.annotation.UnsupportedAppUsage, which libcore's and ICU's sources carry. Soong compiles it into
 * core-all and zaps it from core-icu4j; the boot jars do not contain it, and neither do ours
 * (runtime/art/boot_jars.py). Class retention, as the real one.
 */
package android.compat.annotation;

import java.lang.annotation.Repeatable;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

@Retention(RetentionPolicy.CLASS)
@Repeatable(UnsupportedAppUsage.Container.class)
public @interface UnsupportedAppUsage {
    String expectedSignature() default "";

    int maxTargetSdk() default Integer.MAX_VALUE;

    long trackingBug() default 0;

    String implicitMember() default "";

    String publicAlternatives() default "";

    String overrideSourcePosition() default "";

    @Retention(RetentionPolicy.CLASS)
    @interface Container {
        UnsupportedAppUsage[] value();
    }
}
