// gles-java-test's JNI library (ADR-0040 decision 3: the graphics front libs are public libraries). It links libEGL,
// libGLESv2 and libGLESv3 as Godot's libgodot_android.so does (link.txt), so the class loader's namespace must reach
// them, opens each front lib by its name as Godot's volk opens libvulkan.so, and draws from native code in the
// context Java made current, as Godot's renderer draws in its GLSurfaceView's. Its answers hold on every device.
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <dlfcn.h>
#include <jni.h>
#include <stdio.h>

typedef void (*Function)(void);
typedef Function (*GetInstanceProcAddr)(void *instance, const char *name);

static const char *const kFrontLibs[] = {"libEGL.so", "libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so",
                                         "libvulkan.so"};

// Which front libs dlopen opens by name, and whether libvulkan's vkGetInstanceProcAddr finds vkCreateInstance.
JNIEXPORT jstring JNICALL Java_io_github_kisame76_minato_test_glesjava_GlesJavaApplication_nativeOpen(JNIEnv *env,
                                                                                                    jclass clazz) {
    (void)clazz;
    char text[512];
    size_t used = 0;
    void *vulkan = NULL;
    for (size_t index = 0; index < sizeof kFrontLibs / sizeof *kFrontLibs; index++) {
        void *library = dlopen(kFrontLibs[index], RTLD_NOW);
        used += (size_t)snprintf(text + used, sizeof text - used, "%s%s %s", index == 0 ? "dlopen " : ", ",
                                 kFrontLibs[index], library != NULL ? "opened" : "not opened");
        vulkan = library;  // the last one, libvulkan.so, stays
    }
    GetInstanceProcAddr get = vulkan != NULL ? (GetInstanceProcAddr)dlsym(vulkan, "vkGetInstanceProcAddr") : NULL;
    snprintf(text + used, sizeof text - used, "; vkGetInstanceProcAddr finds vkCreateInstance %s",
             get != NULL && get(NULL, "vkCreateInstance") != NULL ? "true" : "false");
    return (*env)->NewStringUTF(env, text);
}

// A clear to magenta from native code, read back at a pixel center: exact in 8 bits.
JNIEXPORT jstring JNICALL Java_io_github_kisame76_minato_test_glesjava_GlesJavaApplication_nativeDraw(JNIEnv *env,
                                                                                                    jclass clazz) {
    (void)clazz;
    const int current = eglGetCurrentContext() != EGL_NO_CONTEXT;
    glViewport(0, 0, 64, 64);
    glClearColor(1.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    unsigned char pixel[4] = {0, 0, 0, 0};
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    char text[160];
    snprintf(text, sizeof text, "native code in EGL14's context: current %s, clear (32, 32) %d %d %d %d, glGetError %d",
             current ? "true" : "false", pixel[0], pixel[1], pixel[2], pixel[3], (int)glGetError());
    return (*env)->NewStringUTF(env, text);
}
