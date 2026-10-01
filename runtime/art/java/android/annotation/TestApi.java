/*
 * Minato: a compile-time stand-in (ADR-0033 a) for frameworks/base's android.annotation.TestApi, which libcore's
 * sources carry; see SystemApi.java.
 */
package android.annotation;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

@Retention(RetentionPolicy.SOURCE)
public @interface TestApi {}
