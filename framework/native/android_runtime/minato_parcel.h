// android::Parcel as libbinder's Parcel.cpp of android-15.0.0_r1 keeps it, for the parcels of one process (ADR-0043
// d2): android.os.Parcel's natives (android_os_Parcel.cpp) write and read through it in libbinder's wire format, so
// marshall() gives Android's bytes. Minato has no binder driver and no RPC: a binder in a parcel is the Java object
// itself, which the parcel holds by a global reference, and a file descriptor is the parcel's own dup, tagged for
// fdsan as libbinder tags it, so that no one else closes it. Both stand in
// the object list beside their flat_binder_object in the data, and a read takes them from the list, never from bytes
// an app wrote. Every read checks its bounds as libbinder's does: past the end it answers 0 or null, a plain read over
// an object fails, and an object counts only at an offset the list holds. Where libbinder itself would reach past its
// buffer (a write or an append at a position beyond the data, appending a parcel to itself), Minato stays inside it.
// Blobs always stay in the data: there is no ashmem.
#pragma once

#include <jni.h>
#include <linux/android/binder.h>
#include <stddef.h>
#include <stdint.h>
#include <utils/Errors.h>

#include <vector>

namespace android {

class String8;

class Parcel {
public:
    Parcel();
    ~Parcel();
    Parcel(const Parcel &) = delete;
    Parcel &operator=(const Parcel &) = delete;

    // The VM whose global references the parcels hold, recorded at the natives' registration.
    static void setJavaVM(JavaVM *vm);
    static int64_t globalAllocSize();
    static int64_t globalAllocCount();

    const uint8_t *data() const { return mData; }
    // The larger of the data's end and the position, as libbinder's.
    size_t dataSize() const;
    size_t dataAvail() const;
    size_t dataPosition() const { return mDataPos; }
    size_t dataCapacity() const { return mDataCapacity; }
    status_t setDataSize(size_t size);
    void setDataPosition(size_t pos) const;
    status_t setDataCapacity(size_t size);
    bool pushAllowFds(bool allowFds);
    void restoreAllowFds(bool lastValue) { mAllowFds = lastValue; }
    void markSensitive() { mDeallocZero = true; }
    // Releases the objects and the data, as recycle does; the parcel is then empty.
    void freeData();
    size_t objectsCount() const { return mObjects.size(); }

    status_t appendFrom(const Parcel *parcel, size_t offset, size_t len);
    int compareData(const Parcel &other) const;
    status_t compareDataInRange(size_t thisOffset, const Parcel &other, size_t otherOffset, size_t len,
                                int *result) const;
    bool hasFileDescriptors() const;
    status_t hasFileDescriptorsInRange(size_t offset, size_t len, bool *result) const;
    status_t hasBinders(bool *result) const;
    status_t hasBindersInRange(size_t offset, size_t len, bool *result) const;

    // The interface token: the caller's StrictMode policy, the work source (never propagated), libbinder's header for
    // the system image and the descriptor. enforceInterface hands back the policy it read, which the caller sets on
    // the thread whether or not the descriptor matches, as libbinder does.
    status_t writeInterfaceToken(const char16_t *str, size_t len, int32_t strictModePolicy);
    bool enforceInterface(const char16_t *interface, size_t len, int32_t *strictModePolicy) const;
    int32_t readCallingWorkSourceUid() const;
    bool replaceCallingWorkSourceUid(int32_t uid);

    status_t writeInt32(int32_t val);
    status_t writeInt64(int64_t val);
    status_t writeFloat(float val);
    status_t writeDouble(double val);
    // What libinput's MotionEvent parcel takes beside the rest (ADR-0045 step 7b1), as libbinder's: a bool and a byte
    // fill an int32's slot, a uint32 is aligned as an int32, and a byte vector is its length as an int32, then its
    // bytes, padded to 4; a negative length reads as UNEXPECTED_NULL. readFloat also reads into a float, with a status.
    status_t writeBool(bool val);
    status_t writeByte(int8_t val);
    status_t writeUint32(uint32_t val);
    status_t writeByteVector(const std::vector<uint8_t> &val);
    void *writeInplace(size_t len);
    status_t writeString8(const char *str, size_t len);
    // As libbinder's, for libandroidfw's CursorWindow (the compatibility plan's gap 7): a String8 goes in as the form
    // above; readString8 answers UNEXPECTED_NULL and an empty string for a null one; read copies len bytes and moves on
    // by them padded to 4, as readInplace; readUint32 and readBool give a status, as readInt32's form does.
    status_t writeString8(const String8 &str);
    status_t writeString16(const char16_t *str, size_t len);
    // As libbinder's, for libinput's KeyCharacterMap parcel (ADR-0045 step 7c1): a C string is its bytes and NUL,
    // padded to 4; readCString finds the NUL within the data or answers null; errorCheck is the parcel's error.
    status_t writeCString(const char *str);
    // A blob of len bytes, always in place (BLOB_INPLACE), which *outData then points at.
    status_t writeBlob(size_t len, void **outData);
    status_t writeStrongBinder(JNIEnv *env, jobject binder);
    status_t writeDupFileDescriptor(int fd);

    int32_t readInt32() const;
    status_t readInt32(int32_t *out) const;
    int64_t readInt64() const;
    float readFloat() const;
    double readDouble() const;
    status_t readFloat(float *out) const;
    bool readBool() const;
    int8_t readByte() const;
    uint32_t readUint32() const;
    status_t readUint32(uint32_t *out) const;
    status_t readBool(bool *out) const;
    status_t read(void *outData, size_t len) const;
    status_t readString8(String8 *out) const;
    status_t readByteVector(std::vector<uint8_t> *val) const;
    const void *readInplace(size_t len) const;
    const char *readString8Inplace(size_t *outLen) const;
    const char16_t *readString16Inplace(size_t *outLen) const;
    const char *readCString() const;
    status_t errorCheck() const { return mError; }
    status_t readBlob(size_t len, const void **outData) const;
    // A new local reference to the binder at the position, or null.
    jobject readStrongBinder(JNIEnv *env) const;
    // The parcel's own descriptor at the position, which stays the parcel's, or BAD_TYPE.
    int readFileDescriptor() const;

private:
    // An entry of the object list: the offset of its flat_binder_object in the data, the token its cookie carries,
    // and what it stands for, a binder's global reference or the parcel's own descriptor.
    struct Object {
        size_t offset;
        uint64_t token;
        jobject binder;
        int fd;
    };

    void initState();
    void freeDataNoInit();
    void releaseObject(const Object &object) const;
    void releaseObjects();
    status_t finishWrite(size_t len);
    status_t growData(size_t len);
    status_t continueWrite(size_t desired);
    status_t ensureCapacity(size_t len);
    template <typename T>
    status_t writeAligned(T val);
    template <typename T>
    status_t readAligned(T *out) const;
    template <typename T>
    T readAligned() const;
    status_t writeObject(const flat_binder_object &val, bool nullMetaData, const Object &object);
    // Reads the flat_binder_object at the position into *out; for one in the list also its entry, or null when the
    // bytes there are not the entry's. False when there is none to read or it is not in the list.
    bool readObject(bool nullMetaData, flat_binder_object *out, const Object **entry) const;
    status_t validateReadData(size_t upperBound) const;
    void scanForFds() const;
    void updateWorkSourceRequestHeaderPosition() const;

    status_t mError;
    uint8_t *mData;
    size_t mDataSize;
    size_t mDataCapacity;
    mutable size_t mDataPos;
    mutable std::vector<Object> mObjects;
    mutable size_t mNextObjectHint;
    mutable bool mObjectsSorted;
    mutable bool mHasFds;
    mutable bool mFdsKnown;
    mutable bool mRequestHeaderPresent;
    mutable size_t mWorkSourceRequestHeaderPosition;
    bool mAllowFds;
    bool mDeallocZero;
};

}  // namespace android
