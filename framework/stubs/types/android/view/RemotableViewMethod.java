package android.view;

/** Compile-only stub (ADR-0039 step c2c2), never in framework.jar: the RemoteViews annotation on view setters. */
public @interface RemotableViewMethod {
    String asyncImpl() default "";
}
