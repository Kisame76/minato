// ADR-0048 c)'s test (splits-test): the JNI library that the arm64 split holds, which SplitsApplication loads with
// System.loadLibrary from the base module's code; one function answers with a fixed line, the other reads the
// feature split's asset and lists the assets folder through the NDK, from the Java AssetManager's native object.
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <jni.h>
#include <stdio.h>
#include <string.h>

JNIEXPORT jstring JNICALL Java_io_github_kisame76_minato_test_splits_SplitsApplication_nativeAnswer(JNIEnv *env,
                                                                                                    jclass type) {
    (void)type;
    return (*env)->NewStringUTF(env, "the arm64 split's library answers");
}

static void append_asset(AAssetManager *assets, const char *name, char *text, size_t size) {
    AAsset *asset = assets != NULL ? AAssetManager_open(assets, name, AASSET_MODE_STREAMING) : NULL;
    char line[100] = "unreadable";
    int got = asset != NULL ? AAsset_read(asset, line, sizeof line - 1) : -1;
    if (asset != NULL) AAsset_close(asset);
    if (got >= 0) {
        line[got] = 0;
        char *end = strchr(line, '\n');
        if (end != NULL) *end = 0;
    }
    snprintf(text + strlen(text), size - strlen(text), "%s: %s | ", name, line);
}

JNIEXPORT jstring JNICALL Java_io_github_kisame76_minato_test_splits_SplitsApplication_nativeAssets(
    JNIEnv *env, jclass type, jobject manager) {
    (void)type;
    char text[512] = "";
    AAssetManager *assets = AAssetManager_fromJava(env, manager);
    append_asset(assets, "feature.txt", text, sizeof text);
    append_asset(assets, "base.txt", text, sizeof text);
    strcat(text, "folder:");
    AAssetDir *folder = assets != NULL ? AAssetManager_openDir(assets, "") : NULL;
    const char *name;
    while (folder != NULL && (name = AAssetDir_getNextFileName(folder)) != NULL && strlen(text) < 400) {
        strcat(text, " ");
        strcat(text, name);
    }
    if (folder != NULL) AAssetDir_close(folder);
    return (*env)->NewStringUTF(env, text);
}
