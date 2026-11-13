// Stand-in for libbinder's binder/Parcel.h (ADR-0045 step 7b1): libinput's Input.cpp writes and reads a MotionEvent
// through android::Parcel, which in Minato is minato_parcel.h's, libbinder's wire format for the parcels of one
// process (ADR-0043 d2). libandroidfw's CursorWindow.cpp takes it too, and fcntl.h
// with it, which libbinder's header brings through binder/unique_fd.h.
#pragma once

#include <fcntl.h>

#include "../../android_runtime/minato_parcel.h"
