package io.github.kisame76.minato.test.animator;

/**
 * animator-test's object whose properties animators find by name. PropertyValuesHolder looks a setter up and calls
 * it through its natives: one value of int or float, two, four, or another count. Each call is noted in order.
 */
public class Target {
    final StringBuilder calls = new StringBuilder();
    private float level;

    private void note(String call) {
        calls.append(calls.length() == 0 ? "" : " ").append(call);
    }

    public float getLevel() {
        return level;
    }

    public void setLevel(float level) {
        this.level = level;
        note("setLevel(" + level + ")");
    }

    public void setCount(int count) {
        note("setCount(" + count + ")");
    }

    public void setPoint(float x, float y) {
        note("setPoint(" + x + ", " + y + ")");
    }

    public void setRgb(float r, float g, float b) {
        note("setRgb(" + r + ", " + g + ", " + b + ")");
    }

    public void setBox(float left, float top, float right, float bottom) {
        note("setBox(" + left + ", " + top + ", " + right + ", " + bottom + ")");
    }

    public void setSize(int width, int height) {
        note("setSize(" + width + ", " + height + ")");
    }

    public void setTriple(int a, int b, int c) {
        note("setTriple(" + a + ", " + b + ", " + c + ")");
    }

    public void setQuad(int a, int b, int c, int d) {
        note("setQuad(" + a + ", " + b + ", " + c + ", " + d + ")");
    }
}
