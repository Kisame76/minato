// ADR-0050 c3's probe: the JNI library of pad-test's on-demand feature module, in its arm64 split, which SplitCompat
// has to make loadable after the module is installed.
#include <jni.h>

JNIEXPORT jstring JNICALL Java_io_github_kisame76_minato_test_pad_extra_ExtraClass_nativeAnswer(JNIEnv *env,
                                                                                            jclass type) {
    (void)type;
    return (*env)->NewStringUTF(env, "the on-demand module's library answers");
}
