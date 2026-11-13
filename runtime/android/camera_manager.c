// libcamera2ndk.so's camera manager (ADR-0042 b): a manager without cameras, as Android's on a device that
// has none, so that an app that links the library, as Godot's libgodot_android.so does, loads and finds no camera.
// Every other function of the library is a stub that stubgen.py generates and that answers an error.
#include <camera/NdkCameraManager.h>
#include <stdlib.h>

struct ACameraManager {
    int unused;
};

ACameraManager* ACameraManager_create(void) {
    return calloc(1, sizeof(ACameraManager));
}

void ACameraManager_delete(ACameraManager* manager) {
    free(manager);
}

// An empty list, numCameras 0, which ACameraManager_deleteCameraIdList frees.
camera_status_t ACameraManager_getCameraIdList(ACameraManager* manager, ACameraIdList** cameraIdList) {
    if (manager == NULL || cameraIdList == NULL) {
        return ACAMERA_ERROR_INVALID_PARAMETER;
    }
    ACameraIdList* empty = calloc(1, sizeof(ACameraIdList));
    if (empty == NULL) {
        return ACAMERA_ERROR_NOT_ENOUGH_MEMORY;
    }
    *cameraIdList = empty;
    return ACAMERA_OK;
}

void ACameraManager_deleteCameraIdList(ACameraIdList* cameraIdList) {
    free(cameraIdList);
}
