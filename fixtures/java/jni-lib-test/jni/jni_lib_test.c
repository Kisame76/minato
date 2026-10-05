// ADR-0039 step c5.1's test (jni-lib-test): the JNI library that JniLibApplication loads with System.loadLibrary,
// whose one function answers with a fixed line.
#include <jni.h>

JNIEXPORT jstring JNICALL Java_io_github_kisame76_minato_test_jnilib_JniLibApplication_answer(JNIEnv *env,
                                                                                               jclass type) {
    (void)type;
    return (*env)->NewStringUTF(env, "the library answers");
}
