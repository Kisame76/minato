package io.github.kisame76.minato.test.translucentdecor;

/** The opaque variant: an opaque fill on a translucent window must stay shown (the regression this guards against). */
public class OpaqueActivity extends TranslucentDecorActivity {
    @Override
    protected int fillColor() {
        return 0xFFCC3344;
    }
}
