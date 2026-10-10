package android.content.pm.parsing.result;

/** Compile-only stub (the Unity test APK resolve list, batch E), never in framework.jar: the result of the package parser. */
public interface ParseResult<ResultType> {
    ResultType getResult();

    boolean isError();

    int getErrorCode();

    String getErrorMessage();
}
