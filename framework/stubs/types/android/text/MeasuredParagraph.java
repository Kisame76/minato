package android.text;

/**
 * Compile-only stub (ADR-0039 b3.4), never in framework.jar: TextUtils' ellipsize and commaEllipsize measure text
 * with it, which needs Paint's text layout (minikin) that Minato's framework.jar does not have yet.
 */
public class MeasuredParagraph {
    private MeasuredParagraph() {}

    public static MeasuredParagraph buildForMeasurement(TextPaint paint, CharSequence text, int start, int end,
            TextDirectionHeuristic textDir, MeasuredParagraph recycle) {
        throw new RuntimeException("stub");
    }

    public void recycle() {
        throw new RuntimeException("stub");
    }

    public char[] getChars() {
        throw new RuntimeException("stub");
    }

    public float getWholeWidth() {
        throw new RuntimeException("stub");
    }

    public AutoGrowArray.FloatArray getWidths() {
        throw new RuntimeException("stub");
    }

    int breakText(int limit, boolean forwards, float width) {
        throw new RuntimeException("stub");
    }

    float measure(int start, int limit) {
        throw new RuntimeException("stub");
    }
}
