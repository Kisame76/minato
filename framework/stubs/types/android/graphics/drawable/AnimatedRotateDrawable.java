package android.graphics.drawable;

/**
 * Compile-only stub (ADR-0039 step c2c2), never in framework.jar: DrawableInflater makes one for its
 * <animated-rotate> tag.
 */
public class AnimatedRotateDrawable extends DrawableWrapper implements Animatable {
    public AnimatedRotateDrawable() {
        super(null);
        throw new RuntimeException("stub");
    }

    @Override
    public void start() {
        throw new RuntimeException("stub");
    }

    @Override
    public void stop() {
        throw new RuntimeException("stub");
    }

    @Override
    public boolean isRunning() {
        throw new RuntimeException("stub");
    }
}
