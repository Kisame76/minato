package androidx.annotation;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

/** Compile-only stub (ADR-0039 step c2c3), never in framework.jar: WindowOnBackInvokedDispatcher carries it. */
@Retention(RetentionPolicy.CLASS)
public @interface VisibleForTesting {
    int otherwise() default 2;
}
