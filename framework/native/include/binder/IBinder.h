// Stand-in for libbinder's binder/IBinder.h (ADR-0039 b3.3): core/jni's android_util_Binder.h, which
// android_util_AssetManager.cpp includes for newParcelFileDescriptor, declares its conversions with sp<IBinder>, and
// libinput's Input.h copies one in PointerCaptureRequest (ADR-0045 step 7b1), for which the class must be whole.
// Minato has no libbinder (decision 10), so IBinder is an empty RefBase that nothing makes, and nothing here converts
// one. Parcel is declared as libbinder's IBinder.h declares it, which core/jni's android_os_Parcel.h relies on, and
// libutils' Vector comes with it, as with libbinder's, which libinput's KeyCharacterMap.h relies on (ADR-0045 step 7c1).
#pragma once

#include <utils/Errors.h>
#include <utils/RefBase.h>
#include <utils/StrongPointer.h>
#include <utils/Vector.h>

namespace android {
class Parcel;

class IBinder : public virtual RefBase {
protected:
    virtual ~IBinder() = default;
};
}  // namespace android
