// libcodec-test: the JNI half of codec-test (ADR-0033 test 2, fixtures/java/codec-test). Each function holds JNI
// references while Main.collect runs three collections from native code, at each of which SS moves every live object,
// and then checks what the references stand for. Main.sigactionLibrary names the library its sigaction comes from.
#include <dlfcn.h>
#include <jni.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

/// Main.collect(times), from native code.
static void collect(JNIEnv *env, jclass main, jint times) {
    jmethodID method = (*env)->GetStaticMethodID(env, main, "collect", "(I)V");
    (*env)->CallStaticVoidMethod(env, main, method, times);
}

/// Global references to `objects`, held across three collections: how many stand for the same object afterwards.
JNIEXPORT jint JNICALL Java_Main_globalReferences(JNIEnv *env, jclass main, jobjectArray objects) {
    const jsize count = (*env)->GetArrayLength(env, objects);
    jobject *globals = calloc((size_t)count, sizeof(jobject));
    if (globals == NULL) return -1;
    for (jsize i = 0; i < count; i++) {
        jobject local = (*env)->GetObjectArrayElement(env, objects, i);
        globals[i] = (*env)->NewGlobalRef(env, local);
        (*env)->DeleteLocalRef(env, local);
    }
    collect(env, main, 3);
    jint same = 0;
    for (jsize i = 0; i < count; i++) {
        jobject local = (*env)->GetObjectArrayElement(env, objects, i);
        same += (*env)->IsSameObject(env, globals[i], local) ? 1 : 0;
        (*env)->DeleteLocalRef(env, local);
        (*env)->DeleteGlobalRef(env, globals[i]);
    }
    free(globals);
    return same;
}

/// Weak global references to `objects` and to `dead` new objects that nothing else holds, across three collections:
/// result[0] counts those that still stand for their object, result[1] those to the dead objects that were cleared.
JNIEXPORT void JNICALL Java_Main_weakGlobalReferences(JNIEnv *env, jclass main, jobjectArray objects, jint dead,
                                                      jintArray result) {
    const jsize count = (*env)->GetArrayLength(env, objects);
    jweak *live = calloc((size_t)count, sizeof(jweak));
    jweak *gone = calloc((size_t)dead, sizeof(jweak));
    if (live == NULL || gone == NULL) {
        free(live);
        free(gone);
        return;
    }
    jclass object_class = (*env)->FindClass(env, "java/lang/Object");
    for (jsize i = 0; i < count; i++) {
        jobject local = (*env)->GetObjectArrayElement(env, objects, i);
        live[i] = (*env)->NewWeakGlobalRef(env, local);
        (*env)->DeleteLocalRef(env, local);
    }
    for (jint i = 0; i < dead; i++) {
        jobject local = (*env)->AllocObject(env, object_class);
        gone[i] = (*env)->NewWeakGlobalRef(env, local);
        (*env)->DeleteLocalRef(env, local);
    }
    collect(env, main, 3);
    jint counts[2] = {0, 0};
    for (jsize i = 0; i < count; i++) {
        jobject local = (*env)->GetObjectArrayElement(env, objects, i);
        counts[0] += (*env)->IsSameObject(env, live[i], local) ? 1 : 0;
        (*env)->DeleteLocalRef(env, local);
        (*env)->DeleteWeakGlobalRef(env, live[i]);
    }
    for (jint i = 0; i < dead; i++) {
        counts[1] += (*env)->IsSameObject(env, gone[i], NULL) ? 1 : 0;
        (*env)->DeleteWeakGlobalRef(env, gone[i]);
    }
    (*env)->SetIntArrayRegion(env, result, 0, 2, counts);
    free(live);
    free(gone);
}

/// Local references to `objects` in one frame, held across three collections: how many stand for the same object
/// afterwards.
JNIEXPORT jint JNICALL Java_Main_localReferences(JNIEnv *env, jclass main, jobjectArray objects) {
    const jsize count = (*env)->GetArrayLength(env, objects);
    if ((*env)->PushLocalFrame(env, count + 16) != 0) return -1;
    jobject *locals = calloc((size_t)count, sizeof(jobject));
    if (locals == NULL) {
        (*env)->PopLocalFrame(env, NULL);
        return -1;
    }
    for (jsize i = 0; i < count; i++) locals[i] = (*env)->GetObjectArrayElement(env, objects, i);
    collect(env, main, 3);
    jint same = 0;
    for (jsize i = 0; i < count; i++) {
        jobject again = (*env)->GetObjectArrayElement(env, objects, i);
        same += (*env)->IsSameObject(env, locals[i], again) ? 1 : 0;
        (*env)->DeleteLocalRef(env, again);
    }
    free(locals);
    (*env)->PopLocalFrame(env, NULL);
    return same;
}

/// `count` new Main$Node objects that only local references of this frame hold, across three collections: how many
/// still have their index afterwards.
JNIEXPORT jint JNICALL Java_Main_nativeObjects(JNIEnv *env, jclass main, jint count) {
    jclass node_class = (*env)->FindClass(env, "Main$Node");
    jmethodID constructor = (*env)->GetMethodID(env, node_class, "<init>", "(I)V");
    jfieldID index = (*env)->GetFieldID(env, node_class, "index", "I");
    if ((*env)->PushLocalFrame(env, count + 16) != 0) return -1;
    jobject *nodes = calloc((size_t)count, sizeof(jobject));
    if (nodes == NULL) {
        (*env)->PopLocalFrame(env, NULL);
        return -1;
    }
    for (jint i = 0; i < count; i++) nodes[i] = (*env)->NewObject(env, node_class, constructor, i);
    collect(env, main, 3);
    jint intact = 0;
    for (jint i = 0; i < count; i++) intact += (*env)->GetIntField(env, nodes[i], index) == i ? 1 : 0;
    free(nodes);
    (*env)->PopLocalFrame(env, NULL);
    return intact;
}

/// The file name of the library whose sigaction this library's relocation found, as dladdr names it: on Android 15
/// libsigchain.so, the first of the global group that linker64 gives every namespace (ADR-0040 test 2).
JNIEXPORT jstring JNICALL Java_Main_sigactionLibrary(JNIEnv *env, jclass main) {
    (void)main;
    int (*const function)(int, const struct sigaction *, struct sigaction *) = sigaction;
    Dl_info info;
    if (dladdr((const void *)function, &info) == 0 || info.dli_fname == NULL) {
        return (*env)->NewStringUTF(env, "no library");
    }
    const char *slash = strrchr(info.dli_fname, '/');
    return (*env)->NewStringUTF(env, slash != NULL ? slash + 1 : info.dli_fname);
}
