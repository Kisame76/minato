/*
 * Minato: a compile-time stand-in (ADR-0033 a) for frameworks/base's android.annotation.FlaggedApi, which ICU's
 * sources carry with the name of an aconfig flag; see SystemApi.java.
 */
package android.annotation;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

@Retention(RetentionPolicy.SOURCE)
public @interface FlaggedApi {
    String value();
}
