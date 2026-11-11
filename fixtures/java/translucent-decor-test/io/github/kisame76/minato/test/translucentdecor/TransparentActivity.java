package io.github.kisame76.minato.test.translucentdecor;

/**
 * The transparent variant: a fully transparent first frame is the known "nothing to show" case, hidden as transparent.
 */
public class TransparentActivity extends TranslucentDecorActivity {
    @Override
    protected int fillColor() {
        return 0x00000000;
    }
}
