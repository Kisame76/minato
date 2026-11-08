/* gles-procs-test: which GLES, EGL and Vulkan functions an app finds, against Android 15. An engine loads its GL and
   Vulkan tables by dlsym, eglGetProcAddress or vk*ProcAddr before or after its context and calls what it finds
   unchecked; a NULL where Android has a function ends such an app at its first call (an IL2CPP engine's
   glGetStringi through dlsym of libGLESv2.so, 2026-11-05). Each check asks every name of its kind:
   - dlsym:<library>   dlsym of the library that dlopen gives: libEGL, libGLESv1_CM, libGLESv2, libGLESv3, libvulkan;
   - egl:before, egl:none, egl:es1, egl:es2, egl:es3   eglGetProcAddress before eglInitialize, without a context and
     with an ES 1, ES 2 and ES 3 context current on a pbuffer;
   - vk:global, vk:instance, vk:device   vkGetInstanceProcAddr without and with an instance (with VK_KHR_surface and
     VK_KHR_android_surface) and vkGetDeviceProcAddr with a device (with VK_KHR_swapchain and Android's hardware buffer
     extensions where the device has them).
   Built with GLES_PROCS_MEASURE, on the emulator, it prints every name each check finds among all the NDK's
   headers' functions (gles_procs.py --candidates): Android's measurement, graphics/tests/gles_procs_android.txt.
   Built without, it prints for each check how many names of that measurement it does not find (0 on Android) and
   those names, whether dlsym finds glGetStringi in libGLESv2 and libGLESv3, and: in each GLES context the listed
   extensions whose functions eglGetProcAddress lacks, whether glGetStringi lists GL_EXTENSIONS' names, and a call
   of glGetVkProcAddrNV, which no listed extension offers here and Android answers
   with its "called unimplemented OpenGL ES API" entry, returning NULL. Every line is the same on every device.
   SAY prints a line's pieces (printf, or the log in fixtures/apps/gles-procs-test). */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES3/gl3.h>
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SAY
#define SAY printf
#endif
#ifndef GLES_PROCS_MAIN
#define GLES_PROCS_MAIN main
#endif

#define COUNT(array) (sizeof(array) / sizeof *(array))

enum kind { KIND_GL, KIND_EGL, KIND_VK };
struct name {
    const char *name;
    enum kind kind;
    int level;  // Vulkan's: 0 global, 1 instance, 2 device
};
struct extension_name {
    const char *extension;
    const char *name;
};
struct required_name {
    const char *check;
    const char *name;
};

#ifdef GLES_PROCS_MEASURE
static const struct name candidates[] = {
#define GL(name) {#name, KIND_GL, 0},
#define EGL(name) {#name, KIND_EGL, 0},
#define VK(name, level) {#name, KIND_VK, level},
#define EXT(extension, name)
#include "gles_procs_candidates.inc"
#undef GL
#undef EGL
#undef VK
#undef EXT
};
#else
static const struct required_name required[] = {
#define REQUIRED(check, name) {check, #name},
#include "gles_procs_required.inc"
#undef REQUIRED
};
static const struct extension_name extension_names[] = {
#define GL(name)
#define EGL(name)
#define VK(name, level)
#define EXT(extension, name) {extension, #name},
#include "gles_procs_candidates.inc"
#undef GL
#undef EGL
#undef VK
#undef EXT
};
#endif

/* One check's lookup of a name. */
typedef void *(*lookup)(const char *name);
static void *library;
static PFNEGLGETPROCADDRESSPROC get_proc;
static PFN_vkGetInstanceProcAddr instance_proc;
static PFN_vkGetDeviceProcAddr device_proc;
static VkInstance instance;
static VkDevice device;

static void *by_dlsym(const char *name) {
    return dlsym(library, name);
}
static void *by_egl(const char *name) {
    return get_proc != NULL ? (void *)get_proc(name) : NULL;
}
static void *by_vk_global(const char *name) {
    return instance_proc != NULL ? (void *)instance_proc(VK_NULL_HANDLE, name) : NULL;
}
static void *by_vk_instance(const char *name) {
    return instance_proc != NULL && instance != VK_NULL_HANDLE ? (void *)instance_proc(instance, name) : NULL;
}
static void *by_vk_device(const char *name) {
    return device_proc != NULL && device != VK_NULL_HANDLE ? (void *)device_proc(device, name) : NULL;
}

/* The measurement prints "<check> <name>" for each name of the kinds it asks that the lookup finds; the test prints
   "<check>: NULL count <n>" for the measured names it does not find, then those names, 30 a line, as
   "<check>: NULL: <names>" (a device's log cuts a line at about 4000 bytes). */
static void check(const char *name_of_check, lookup find, unsigned kinds, int vk_level) {
#ifdef GLES_PROCS_MEASURE
    for (size_t index = 0; index < COUNT(candidates); index++) {
        const struct name *entry = &candidates[index];
        if (!(kinds & (1u << entry->kind))) continue;
        if (entry->kind == KIND_VK && vk_level >= 0 && entry->level != vk_level) continue;
        if (find(entry->name) != NULL) SAY("%s %s\n", name_of_check, entry->name);
    }
#else
    (void)kinds;
    (void)vk_level;
    int missing = 0;
    for (size_t index = 0; index < COUNT(required); index++) {
        if (strcmp(required[index].check, name_of_check) == 0 && find(required[index].name) == NULL) missing++;
    }
    SAY("%s: NULL count %d\n", name_of_check, missing);
    int in_line = 0;
    for (size_t index = 0; index < COUNT(required); index++) {
        if (strcmp(required[index].check, name_of_check) != 0 || find(required[index].name) != NULL) continue;
        if (in_line == 0) SAY("%s: NULL:", name_of_check);
        SAY(" %s", required[index].name);
        if (++in_line == 30) {
            SAY("\n");
            in_line = 0;
        }
    }
    if (in_line > 0) SAY("\n");
#endif
}

#define GL_KINDS (1u << KIND_GL)
#define EGL_KINDS ((1u << KIND_GL) | (1u << KIND_EGL))

static void dlsym_check(const char *soname, const char *name_of_check, unsigned kinds) {
    library = dlopen(soname, RTLD_NOW);
    if (library == NULL) {
        SAY("%s: dlopen failed\n", name_of_check);
        return;
    }
    check(name_of_check, by_dlsym, kinds, -1);
#ifndef GLES_PROCS_MEASURE
    // The function whose NULL ended an engine at its start, on a line of its own.
    if (strcmp(soname, "libGLESv2.so") == 0 || strcmp(soname, "libGLESv3.so") == 0) {
        SAY("%s: glGetStringi %s\n", name_of_check, by_dlsym("glGetStringi") != NULL ? "found" : "NULL");
    }
#endif
}

#ifndef GLES_PROCS_MEASURE
/* Whether `name` is a word of the space-separated `list`. */
static int listed(const char *list, const char *name) {
    const size_t length = strlen(name);
    for (const char *at = list; (at = strstr(at, name)) != NULL; at += length) {
        if ((at == list || at[-1] == ' ') && (at[length] == ' ' || at[length] == '\0')) return 1;
    }
    return 0;
}

static void context_lines(const char *what) {
    PFNGLGETSTRINGPROC get_string = (PFNGLGETSTRINGPROC)get_proc("glGetString");
    const char *version = get_string != NULL ? (const char *)get_string(GL_VERSION) : NULL;
    const char *list = get_string != NULL ? (const char *)get_string(GL_EXTENSIONS) : NULL;
    if (list == NULL) {
        SAY("%s: GL_EXTENSIONS NULL\n", what);
        return;
    }
    int gaps = 0;
    for (size_t index = 0; index < COUNT(extension_names); index++) {
        if (listed(list, extension_names[index].extension) && get_proc(extension_names[index].name) == NULL) {
            SAY("%s: lists %s, NULL %s\n", what, extension_names[index].extension, extension_names[index].name);
            gaps++;
        }
    }
    SAY("%s: listed extensions' functions NULL: %d\n", what, gaps);
    PFNGLGETSTRINGIPROC get_stringi = (PFNGLGETSTRINGIPROC)get_proc("glGetStringi");
    PFNGLGETINTEGERVPROC get_integer = (PFNGLGETINTEGERVPROC)get_proc("glGetIntegerv");
    if (version != NULL && strstr(version, "OpenGL ES 3") != NULL && get_stringi != NULL && get_integer != NULL) {
        GLint count = 0;
        get_integer(GL_NUM_EXTENSIONS, &count);
        int same = count > 0;
        for (GLint index = 0; index < count; index++) {
            const char *name = (const char *)get_stringi(GL_EXTENSIONS, (GLuint)index);
            if (name == NULL || !listed(list, name)) same = 0;
        }
        SAY("%s: glGetStringi lists GL_EXTENSIONS' names %s\n", what, same ? "true" : "false");
    }
}
#endif

/* An ES `version` context current on a pbuffer of a config for that version: its check, and its lines. */
static void in_context(EGLDisplay display, int version, const char *name) {
    const EGLint renderable = version == 1 ? EGL_OPENGL_ES_BIT : version == 2 ? EGL_OPENGL_ES2_BIT
                                                                              : EGL_OPENGL_ES3_BIT_KHR;
    const EGLint spec[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                           EGL_RENDERABLE_TYPE, renderable, EGL_NONE};
    EGLConfig config;
    EGLint configs = 0;
    eglChooseConfig(display, spec, &config, 1, &configs);
    const EGLint size[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    EGLSurface pbuffer = configs > 0 ? eglCreatePbufferSurface(display, config, size) : EGL_NO_SURFACE;
    const EGLint attributes[] = {EGL_CONTEXT_CLIENT_VERSION, version, EGL_NONE};
    EGLContext context = pbuffer != EGL_NO_SURFACE
                             ? eglCreateContext(display, config, EGL_NO_CONTEXT, attributes) : EGL_NO_CONTEXT;
    if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, pbuffer, pbuffer, context)) {
        SAY("%s: no context, 0x%x\n", name, eglGetError());
        if (pbuffer != EGL_NO_SURFACE) eglDestroySurface(display, pbuffer);
        return;
    }
    check(name, by_egl, EGL_KINDS, -1);
#ifndef GLES_PROCS_MEASURE
    if (version >= 2) context_lines(name);
#endif
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, pbuffer);
}

static void vulkan(void) {
    void *loader = dlopen("libvulkan.so", RTLD_NOW);
    instance_proc = loader != NULL ? (PFN_vkGetInstanceProcAddr)dlsym(loader, "vkGetInstanceProcAddr") : NULL;
    if (instance_proc == NULL) {
        SAY("vulkan: no vkGetInstanceProcAddr\n");
        return;
    }
    check("vk:global", by_vk_global, 1u << KIND_VK, 0);
    PFN_vkCreateInstance create_instance = (PFN_vkCreateInstance)instance_proc(VK_NULL_HANDLE, "vkCreateInstance");
    const char *instance_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    const VkApplicationInfo application = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                           .apiVersion = VK_API_VERSION_1_3};
    const VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                                .pApplicationInfo = &application,
                                                .enabledExtensionCount = 2,
                                                .ppEnabledExtensionNames = instance_extensions};
    if (create_instance == NULL || create_instance(&instance_info, NULL, &instance) != VK_SUCCESS) {
        SAY("vulkan: no instance\n");
        return;
    }
    check("vk:instance", by_vk_instance, 1u << KIND_VK, 1);
    PFN_vkEnumeratePhysicalDevices enumerate =
        (PFN_vkEnumeratePhysicalDevices)instance_proc(instance, "vkEnumeratePhysicalDevices");
    PFN_vkEnumerateDeviceExtensionProperties properties =
        (PFN_vkEnumerateDeviceExtensionProperties)instance_proc(instance, "vkEnumerateDeviceExtensionProperties");
    PFN_vkCreateDevice create_device = (PFN_vkCreateDevice)instance_proc(instance, "vkCreateDevice");
    device_proc = (PFN_vkGetDeviceProcAddr)instance_proc(instance, "vkGetDeviceProcAddr");
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t count = 1;
    if (enumerate == NULL || properties == NULL || enumerate(instance, &count, &physical) < 0 || count == 0) {
        SAY("vulkan: no physical device\n");
        return;
    }
    /* The device's extensions among those wanted: the swapchain and Android's hardware buffers. */
    const char *wanted[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                            VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
                            VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
                            VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME};
    const char *enabled[COUNT(wanted)];
    uint32_t enabled_count = 0;
    uint32_t available = 0;
    properties(physical, NULL, &available, NULL);
    VkExtensionProperties *list = calloc(available + 1, sizeof *list);
    properties(physical, NULL, &available, list);
    for (size_t want = 0; want < COUNT(wanted); want++) {
        for (uint32_t index = 0; index < available; index++) {
            if (strcmp(list[index].extensionName, wanted[want]) == 0) enabled[enabled_count++] = wanted[want];
        }
    }
    free(list);
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                           .queueFamilyIndex = 0,
                                           .queueCount = 1,
                                           .pQueuePriorities = &priority};
    const VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                            .queueCreateInfoCount = 1,
                                            .pQueueCreateInfos = &queue,
                                            .enabledExtensionCount = enabled_count,
                                            .ppEnabledExtensionNames = enabled};
    if (create_device == NULL || device_proc == NULL ||
        create_device(physical, &device_info, NULL, &device) != VK_SUCCESS) {
        SAY("vulkan: no device\n");
        return;
    }
    check("vk:device", by_vk_device, 1u << KIND_VK, 2);
    ((PFN_vkDestroyDevice)device_proc(device, "vkDestroyDevice"))(device, NULL);
    ((PFN_vkDestroyInstance)instance_proc(instance, "vkDestroyInstance"))(instance, NULL);
}

int GLES_PROCS_MAIN(void) {
    dlsym_check("libEGL.so", "dlsym:libEGL", 1u << KIND_EGL);
    get_proc = (PFNEGLGETPROCADDRESSPROC)dlsym(library, "eglGetProcAddress");
    dlsym_check("libGLESv1_CM.so", "dlsym:libGLESv1_CM", GL_KINDS);
    dlsym_check("libGLESv2.so", "dlsym:libGLESv2", GL_KINDS);
    dlsym_check("libGLESv3.so", "dlsym:libGLESv3", GL_KINDS);
    check("egl:before", by_egl, EGL_KINDS, -1);

    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(display, NULL, NULL);
    check("egl:none", by_egl, EGL_KINDS, -1);
    in_context(display, 1, "egl:es1");
    in_context(display, 2, "egl:es2");
    in_context(display, 3, "egl:es3");
#ifndef GLES_PROCS_MEASURE
    void *(*vk_proc)(const char *) = (void *(*)(const char *))get_proc("glGetVkProcAddrNV");
    SAY("glGetVkProcAddrNV: %s", vk_proc != NULL ? "found" : "NULL");
    if (vk_proc != NULL) SAY(", called: %s", vk_proc("vkCreateInstance") != NULL ? "a function" : "NULL");
    SAY("\n");
#endif
    eglTerminate(display);

    dlsym_check("libvulkan.so", "dlsym:libvulkan", 1u << KIND_VK);
    vulkan();
    SAY("done\n");
    return 0;
}
