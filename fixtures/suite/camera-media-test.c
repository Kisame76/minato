// camera-media-test (ADR-0042 b, gap 15): libcamera2ndk.so and libmediandk.so as Godot's libgodot_android.so
// links them. Both load, every function libgodot_android.so imports from them is there, the camera manager has
// no camera, and an image reader of YUV_420_888 is made (ADR-0058 e1), G3c), though no producer can dequeue for it
// (G3a's YUV limit). Opened with dlopen, so that their absence shows at run time.
#include <camera/NdkCameraManager.h>
#include <dlfcn.h>
#include <media/NdkImageReader.h>
#include <stdio.h>

// What libgodot_android.so of Godot 4.7.2 imports from each (llvm-nm -D --undefined-only).
static const char* const camera_names[] = {
    "ACameraCaptureSession_close", "ACameraCaptureSession_setRepeatingRequest", "ACameraCaptureSession_stopRepeating",
    "ACameraDevice_close", "ACameraDevice_createCaptureRequest", "ACameraDevice_createCaptureSession",
    "ACameraManager_create", "ACameraManager_delete", "ACameraManager_deleteCameraIdList",
    "ACameraManager_getCameraCharacteristics", "ACameraManager_getCameraIdList", "ACameraManager_openCamera",
    "ACameraMetadata_free", "ACameraMetadata_getConstEntry", "ACameraOutputTarget_create", "ACameraOutputTarget_free",
    "ACaptureRequest_addTarget", "ACaptureRequest_free", "ACaptureSessionOutputContainer_add",
    "ACaptureSessionOutputContainer_create", "ACaptureSessionOutputContainer_free", "ACaptureSessionOutput_create",
    "ACaptureSessionOutput_free",
};
static const char* const media_names[] = {
    "AImageReader_acquireNextImage", "AImageReader_delete", "AImageReader_getWindow", "AImageReader_new",
    "AImageReader_setImageListener", "AImage_delete", "AImage_getPlaneData", "AImage_getPlanePixelStride",
    "AImage_getPlaneRowStride",
};

static int count_missing(void* library, const char* const* names, size_t count) {
    int missing = 0;
    for (size_t i = 0; i < count; ++i) {
        if (dlsym(library, names[i]) == NULL) {
            printf("missing: %s\n", names[i]);
            ++missing;
        }
    }
    return missing;
}

int main(void) {
    void* camera = dlopen("libcamera2ndk.so", RTLD_NOW);
    printf("libcamera2ndk.so: %s\n", camera != NULL ? "loaded" : "not loaded");
    void* media = dlopen("libmediandk.so", RTLD_NOW);
    printf("libmediandk.so: %s\n", media != NULL ? "loaded" : "not loaded");
    if (camera == NULL || media == NULL) {
        return 1;
    }
    int missing = count_missing(camera, camera_names, sizeof camera_names / sizeof camera_names[0]) +
                  count_missing(media, media_names, sizeof media_names / sizeof media_names[0]);
    printf("libgodot_android.so's 23 camera and 9 image functions: %s\n", missing == 0 ? "all there" : "not all");
    if (missing != 0) {
        return 1;
    }

    __typeof__(ACameraManager_create)* create = dlsym(camera, "ACameraManager_create");
    __typeof__(ACameraManager_getCameraIdList)* id_list = dlsym(camera, "ACameraManager_getCameraIdList");
    __typeof__(ACameraManager_deleteCameraIdList)* delete_id_list = dlsym(camera, "ACameraManager_deleteCameraIdList");
    __typeof__(ACameraManager_openCamera)* open_camera = dlsym(camera, "ACameraManager_openCamera");
    __typeof__(ACameraManager_delete)* delete_manager = dlsym(camera, "ACameraManager_delete");
    ACameraManager* manager = create();
    printf("camera manager: %s\n", manager != NULL ? "made" : "NULL");
    ACameraIdList* ids = NULL;
    camera_status_t status = id_list(manager, &ids);
    printf("camera IDs: status %d, %d cameras\n", status, ids != NULL ? ids->numCameras : -1);
    printf("camera IDs without a list to fill: status %d\n", id_list(manager, NULL));
    delete_id_list(ids);
    ACameraDevice* device = NULL;
    ACameraDevice_StateCallbacks callbacks = {0};
    printf("openCamera of \"0\": status %d, %s\n", open_camera(manager, "0", &callbacks, &device),
           device == NULL ? "no device" : "a device");
    delete_manager(manager);

    __typeof__(AImageReader_new)* new_reader = dlsym(media, "AImageReader_new");
    AImageReader* reader = NULL;
    media_status_t made = new_reader(640, 480, AIMAGE_FORMAT_YUV_420_888, 2, &reader);
    printf("AImageReader_new: status %d, %s\n", made, reader == NULL ? "no reader" : "a reader");
    if (reader != NULL) {
        ((__typeof__(AImageReader_delete)*)dlsym(media, "AImageReader_delete"))(reader);
    }
    return 0;
}
