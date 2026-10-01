/*
 * Minato: a compile-time stand-in (ADR-0033 a) for frameworks/base's android.annotation.SystemApi, which libcore's
 * and ICU's sources carry. Soong compiles the boot jars against core-all, which holds the real one; the jars do not
 * contain it, and neither do ours (runtime/art/boot_jars.py). Source retention, as the real one.
 */
package android.annotation;

import java.lang.annotation.Repeatable;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

@Retention(RetentionPolicy.SOURCE)
@Repeatable(SystemApi.Container.class)
public @interface SystemApi {
    enum Client {
        PRIVILEGED_APPS,
        MODULE_LIBRARIES,
        SYSTEM_SERVER,
    }

    Client client() default Client.PRIVILEGED_APPS;

    @Retention(RetentionPolicy.SOURCE)
    @interface Container {
        SystemApi[] value();
    }
}
