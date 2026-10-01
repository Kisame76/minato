package android.compat.annotation;

import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.Target;

/**
 * Compile-only stub (ADR-0039 b3.1), never in framework.jar: ActivityInfo's compat changes carry it (tools/platform-
 * compat).
 */
@Retention(RetentionPolicy.CLASS)
@Target(ElementType.FIELD)
public @interface Overridable {
}
