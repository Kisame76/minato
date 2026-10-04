package android.view;

/**
 * Compile-only stub (ADR-0039 step c2c2), never in framework.jar: View's accessibility text traversal
 * (getIteratorForGranularity) makes its iterators; accessibility is off (ADR-0041).
 */
public final class AccessibilityIterators {
    public interface TextSegmentIterator {
        int[] following(int current);

        int[] preceding(int current);
    }

    public abstract static class AbstractTextSegmentIterator implements TextSegmentIterator {
        public void initialize(String text) {
            throw new RuntimeException("stub");
        }
    }

    public static class CharacterTextSegmentIterator extends AbstractTextSegmentIterator {
        public static CharacterTextSegmentIterator getInstance(java.util.Locale locale) {
            throw new RuntimeException("stub");
        }

        public int[] following(int offset) {
            throw new RuntimeException("stub");
        }

        public int[] preceding(int offset) {
            throw new RuntimeException("stub");
        }
    }

    public static class WordTextSegmentIterator extends CharacterTextSegmentIterator {
        public static WordTextSegmentIterator getInstance(java.util.Locale locale) {
            throw new RuntimeException("stub");
        }
    }

    public static class ParagraphTextSegmentIterator extends AbstractTextSegmentIterator {
        public static ParagraphTextSegmentIterator getInstance() {
            throw new RuntimeException("stub");
        }

        public int[] following(int offset) {
            throw new RuntimeException("stub");
        }

        public int[] preceding(int offset) {
            throw new RuntimeException("stub");
        }
    }
}
