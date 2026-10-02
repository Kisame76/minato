// Stand-in for libbinder's binder/IBinder.h (ADR-0039 b3.3): core/jni's android_util_Binder.h, which
// android_util_AssetManager.cpp includes for newParcelFileDescriptor, declares its conversions with sp<IBinder>.
// Minato has no libbinder (decision 10), so IBinder is only declared, and nothing here converts one.
#pragma once

#include <utils/Errors.h>
#include <utils/StrongPointer.h>

namespace android {
class IBinder;
}  // namespace android
