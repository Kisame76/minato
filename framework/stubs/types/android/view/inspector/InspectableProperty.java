package android.view.inspector;

/**
 * Compile-only stub (ADR-0039 step c2c2), never in framework.jar: the views' layout inspector annotation (source
 * retention), which View, ViewGroup, FrameLayout and LinearLayout carry.
 */
public @interface InspectableProperty {
    String name() default "";

    int attributeId() default 0;

    boolean hasAttributeId() default true;

    ValueType valueType() default ValueType.INFERRED;

    EnumEntry[] enumMapping() default {};

    FlagEntry[] flagMapping() default {};

    @interface EnumEntry {
        String name();

        int value();
    }

    @interface FlagEntry {
        String name();

        int target();

        int mask() default 0;
    }

    enum ValueType {
        NONE, INFERRED, INT_ENUM, INT_FLAG, COLOR, GRAVITY, RESOURCE_ID
    }
}
