package android.view;

/**
 * Compile-only stub (ADR-0039 step c2c2), never in framework.jar: DisplayCutout parses a cutout's path from its
 * specification (fromResources); a display without a cutout spec never does.
 */
public class CutoutSpecification {
    public android.graphics.Path getPath() {
        throw new RuntimeException("stub");
    }

    public android.graphics.Rect getSafeInset() {
        throw new RuntimeException("stub");
    }

    public android.graphics.Rect getLeftBound() {
        throw new RuntimeException("stub");
    }

    public android.graphics.Rect getTopBound() {
        throw new RuntimeException("stub");
    }

    public android.graphics.Rect getRightBound() {
        throw new RuntimeException("stub");
    }

    public android.graphics.Rect getBottomBound() {
        throw new RuntimeException("stub");
    }

    public static class Parser {
        public Parser(float density, int physicalDisplayWidth, int physicalDisplayHeight,
                float physicalPixelDisplaySizeRatio) {
            throw new RuntimeException("stub");
        }

        public CutoutSpecification parse(String specification) {
            throw new RuntimeException("stub");
        }
    }
}
