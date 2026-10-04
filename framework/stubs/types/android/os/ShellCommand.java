package android.os;

/**
 * Compile-only stub (ADR-0039 step c2c1), never in framework.jar: Intent.parseCommandArgs reads the am shell
 * command's options; no shell runs in an app process.
 */
public abstract class ShellCommand {
    public String peekNextArg() {
        throw new RuntimeException("stub");
    }

    public String getNextOption() {
        throw new RuntimeException("stub");
    }

    public String getNextArg() {
        throw new RuntimeException("stub");
    }

    public String getNextArgRequired() {
        throw new RuntimeException("stub");
    }
}
