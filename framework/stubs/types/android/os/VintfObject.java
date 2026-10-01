package android.os;

/** Compile-only stub (ADR-0039 b3.1), never in framework.jar: Build.isBuildConsistent calls it. */
public class VintfObject {
    public static int verifyBuildAtBoot() {
        throw new RuntimeException("stub");
    }
}
