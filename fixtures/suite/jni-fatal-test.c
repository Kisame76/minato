// jni-fatal-test: the JNI stub's FatalError (ADR-0028, decision 8) logs the app's message and aborts, as ART
// does, so an app that gives up does not go on and its reason is in the log. A test of the runtime: Android's
// JNI is ART's.
// Expected: status 134 (SIGABRT) and fixtures/expected/jni-fatal-test.err; nothing on stdout.
#include <stdio.h>

#include "jni_stub.h"

int main(void) {
    JavaVM *vm = NULL;
    JNIEnv *env = NULL;
    jobject clazz = NULL;
    minato_jni_stub(&vm, &env, &clazz);
    (*env)->FatalError(env, "the app gives up");
    printf("FatalError returned\n");
    return 0;
}
