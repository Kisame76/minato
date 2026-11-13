package androidx.annotation;

/** Compile-only stub (ADR-0045): the AndroidX annotation that Icon's source names. */
@java.lang.annotation.Retention(java.lang.annotation.RetentionPolicy.CLASS)
public @interface RequiresPermission {
    String value() default "";

    String[] allOf() default {};

    String[] anyOf() default {};
}
