// ADR-0043 d2: libbinder's Parcel for the parcels of one process (minato_parcel.h). Each function follows its namesake
// in android-15.0.0_r1's libs/binder/Parcel.cpp, and the comments name where Minato differs.
#define LOG_TAG "Parcel"

#include "minato_parcel.h"

#include <android/fdsan.h>
#include <errno.h>
#include <fcntl.h>
#include <log/log.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <utils/String8.h>

#include <algorithm>
#include <atomic>
#include <limits>

namespace android {
namespace {

// libbinder's header for the system image, B_PACK_CHARS('S', 'Y', 'S', 'T').
constexpr int32_t kHeader = 0x53595354;
constexpr int32_t kStrictModePenaltyGather = INT32_MIN;  // 1 << 31
constexpr int32_t kUnsetWorkSource = -1;
// Stability::Level: a local binder carries SYSTEM, its compilation unit's, and a null binder UNDECLARED.
constexpr int32_t kStabilityUndeclared = 0;
constexpr int32_t kStabilitySystem = 0b001100;
// schedPolicyMask(SCHED_NORMAL, 19): an app's process keeps background scheduling.
constexpr uint32_t kSchedBits = 19;
constexpr int32_t kBlobInplace = 0;

JavaVM *gVm;
std::atomic<int64_t> gGlobalAllocSize;
std::atomic<int64_t> gGlobalAllocCount;
// The tokens in the objects' cookies; never 0, which marks a null binder, or 1, libbinder's cookie of a descriptor the
// parcel owns.
std::atomic<uint64_t> gNextToken{2};

size_t padSize(size_t s) {
    LOG_ALWAYS_FATAL_IF(s > std::numeric_limits<size_t>::max() - 3, "pad size too big %zu", s);
    return (s + 3) & ~size_t{3};
}

// A sensitive parcel's bytes are zeroed before their buffer is freed; the barrier keeps the compiler from dropping
// the stores.
void zeroMemory(uint8_t *data, size_t size) {
    if (data == nullptr) return;
    memset(data, 0, size);
    __asm__ __volatile__("" : : "r"(data) : "memory");
}

// As libbinder's: a sensitive parcel's buffer moves instead of growing in place, so that its old bytes are zeroed.
uint8_t *reallocZeroFree(uint8_t *data, size_t oldCapacity, size_t newCapacity, bool zero) {
    if (!zero) return static_cast<uint8_t *>(realloc(data, newCapacity));
    auto *newData = static_cast<uint8_t *>(malloc(newCapacity));
    if (newData == nullptr) return nullptr;
    memcpy(newData, data, std::min(oldCapacity, newCapacity));
    zeroMemory(data, oldCapacity);
    free(data);
    return newData;
}

// libbinder's fdsan ownership (FdTag, FdTagClose): a descriptor the parcel owns carries the parcel's tag, so that
// only the parcel closes it, and fdsan stops anyone else who would.
uint64_t parcelTag(const void *owner) {
    return android_fdsan_create_owner_tag(ANDROID_FDSAN_OWNER_TYPE_PARCEL, reinterpret_cast<uint64_t>(owner));
}

void fdTag(int fd, const void *oldOwner, const void *newOwner) {
    android_fdsan_exchange_owner_tag(fd, parcelTag(oldOwner), parcelTag(newOwner));
}

void fdTagClose(int fd, const void *owner) {
    android_fdsan_close_with_tag(fd, parcelTag(owner));
}

JNIEnv *currentEnv() {
    JNIEnv *env = nullptr;
    if (gVm == nullptr || gVm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return nullptr;
    }
    return env;
}

}  // namespace

void Parcel::setJavaVM(JavaVM *vm) {
    gVm = vm;
}

int64_t Parcel::globalAllocSize() {
    return gGlobalAllocSize;
}

int64_t Parcel::globalAllocCount() {
    return gGlobalAllocCount;
}

Parcel::Parcel() {
    initState();
}

Parcel::~Parcel() {
    freeDataNoInit();
}

size_t Parcel::dataSize() const {
    return mDataSize > mDataPos ? mDataSize : mDataPos;
}

size_t Parcel::dataAvail() const {
    const size_t result = dataSize() - dataPosition();
    LOG_ALWAYS_FATAL_IF(result > INT32_MAX, "result too big: %zu", result);
    return result;
}

status_t Parcel::setDataSize(size_t size) {
    if (size > INT32_MAX) return BAD_VALUE;
    const status_t err = continueWrite(size);
    if (err == NO_ERROR) mDataSize = size;
    return err;
}

void Parcel::setDataPosition(size_t pos) const {
    LOG_ALWAYS_FATAL_IF(pos > INT32_MAX, "pos too big: %zu", pos);
    mDataPos = pos;
    mNextObjectHint = 0;
    mObjectsSorted = false;
}

status_t Parcel::setDataCapacity(size_t size) {
    if (size > INT32_MAX) return BAD_VALUE;
    if (size > mDataCapacity) return continueWrite(size);
    return NO_ERROR;
}

bool Parcel::pushAllowFds(bool allowFds) {
    const bool origValue = mAllowFds;
    if (!allowFds) mAllowFds = false;
    return origValue;
}

void Parcel::freeData() {
    freeDataNoInit();
    initState();
}

void Parcel::initState() {
    mError = NO_ERROR;
    mData = nullptr;
    mDataSize = 0;
    mDataCapacity = 0;
    mDataPos = 0;
    mObjects.clear();
    mNextObjectHint = 0;
    mObjectsSorted = false;
    mHasFds = false;
    mFdsKnown = true;
    mRequestHeaderPresent = false;
    mWorkSourceRequestHeaderPosition = 0;
    mAllowFds = true;
    mDeallocZero = false;
}

void Parcel::freeDataNoInit() {
    releaseObjects();
    if (mData != nullptr) {
        gGlobalAllocSize -= mDataCapacity;
        gGlobalAllocCount--;
        if (mDeallocZero) zeroMemory(mData, mDataSize);
        free(mData);
    }
}

void Parcel::releaseObject(const Object &object) const {
    if (object.binder != nullptr) {
        if (JNIEnv *env = currentEnv()) {
            env->DeleteGlobalRef(object.binder);
        } else {
            ALOGE("Parcel %p cannot release a binder off the VM's threads", this);
        }
    } else if (object.fd >= 0) {
        fdTagClose(object.fd, this);
    }
}

void Parcel::releaseObjects() {
    for (size_t i = mObjects.size(); i > 0; i--) releaseObject(mObjects[i - 1]);
    mObjects.clear();
}

status_t Parcel::finishWrite(size_t len) {
    if (len > INT32_MAX) return BAD_VALUE;
    mDataPos += len;
    if (mDataPos > mDataSize) mDataSize = mDataPos;
    return NO_ERROR;
}

status_t Parcel::growData(size_t len) {
    if (len > INT32_MAX) return BAD_VALUE;
    // libbinder grows from mDataSize, so a write at a position beyond the data's end could pass the new end of its
    // buffer. Minato grows from the larger of the two, the same as libbinder whenever the position is in the data.
    const size_t end = dataSize();
    if (len > std::numeric_limits<size_t>::max() - end) return NO_MEMORY;
    if (end + len > std::numeric_limits<size_t>::max() / 3) return NO_MEMORY;
    const size_t newSize = ((end + len) * 3) / 2;
    return newSize <= end ? NO_MEMORY : continueWrite(std::max(newSize, size_t{128}));
}

status_t Parcel::ensureCapacity(size_t len) {
    if (mDataPos + len <= mDataCapacity) return NO_ERROR;
    if (const status_t err = growData(len); err != NO_ERROR) return err;
    return mDataPos + len <= mDataCapacity ? NO_ERROR : NO_MEMORY;
}

status_t Parcel::continueWrite(size_t desired) {
    if (desired > INT32_MAX) return BAD_VALUE;

    // Shrinking drops the objects at the end of the list that lie at or after the new end.
    size_t objectsSize = mObjects.size();
    if (desired < mDataSize) {
        if (desired == 0) {
            objectsSize = 0;
        } else {
            while (objectsSize > 0 && mObjects[objectsSize - 1].offset >= desired) objectsSize--;
        }
    }

    if (mData != nullptr) {
        if (objectsSize < mObjects.size()) {
            for (size_t i = objectsSize; i < mObjects.size(); i++) {
                // A dropped descriptor may have been the only one: rescan when asked.
                if (mObjects[i].binder == nullptr) mFdsKnown = false;
                releaseObject(mObjects[i]);
            }
            mObjects.resize(objectsSize);
            mNextObjectHint = 0;
            mObjectsSorted = false;
        }
        if (desired > mDataCapacity) {
            auto *data = reallocZeroFree(mData, mDataCapacity, desired, mDeallocZero);
            if (data == nullptr) {
                mError = NO_MEMORY;
                return NO_MEMORY;
            }
            // New capacity starts zeroed, where libbinder's realloc leaves the heap's old bytes for marshall to show.
            memset(data + mDataCapacity, 0, desired - mDataCapacity);
            gGlobalAllocSize += desired - mDataCapacity;
            mData = data;
            mDataCapacity = desired;
        } else {
            if (mDataSize > desired) mDataSize = desired;
            if (mDataPos > desired) mDataPos = desired;
        }
    } else {
        // The first data.
        auto *data = static_cast<uint8_t *>(calloc(desired > 0 ? desired : 1, 1));
        if (data == nullptr) {
            mError = NO_MEMORY;
            return NO_MEMORY;
        }
        gGlobalAllocSize += desired;
        gGlobalAllocCount++;
        mData = data;
        mDataSize = mDataPos = 0;
        mDataCapacity = desired;
    }
    return NO_ERROR;
}

template <typename T>
status_t Parcel::writeAligned(T val) {
    static_assert(sizeof(T) % 4 == 0);
    if (const status_t err = ensureCapacity(sizeof(val)); err != NO_ERROR) return err;
    memcpy(mData + mDataPos, &val, sizeof(val));
    return finishWrite(sizeof(val));
}

status_t Parcel::writeInt32(int32_t val) {
    return writeAligned(val);
}

status_t Parcel::writeInt64(int64_t val) {
    return writeAligned(val);
}

status_t Parcel::writeFloat(float val) {
    return writeAligned(val);
}

status_t Parcel::writeDouble(double val) {
    return writeAligned(val);
}

status_t Parcel::writeBool(bool val) {
    return writeInt32(int32_t(val));
}

status_t Parcel::writeByte(int8_t val) {
    return writeInt32(int32_t(val));
}

status_t Parcel::writeUint32(uint32_t val) {
    return writeAligned(val);
}

// As libbinder's writeData for a vector of bytes: the length, whose status it does not check, then write()'s bytes.
status_t Parcel::writeByteVector(const std::vector<uint8_t> &val) {
    if (val.size() > static_cast<size_t>(INT32_MAX)) return BAD_VALUE;
    writeInt32(static_cast<int32_t>(val.size()));
    void *const data = writeInplace(val.size());
    if (data == nullptr) return mError;
    if (!val.empty()) memcpy(data, val.data(), val.size());
    return NO_ERROR;
}

void *Parcel::writeInplace(size_t len) {
    if (len > INT32_MAX) return nullptr;
    const size_t padded = padSize(len);
    if (mDataPos + padded < mDataPos) return nullptr;
    if (ensureCapacity(padded) != NO_ERROR) return nullptr;
    uint8_t *const data = mData + mDataPos;
    // The padding is zeroed, as libbinder's mask does.
    if (padded != len) memset(data + len, 0, padded - len);
    finishWrite(padded);
    return data;
}

status_t Parcel::writeString8(const char *str, size_t len) {
    if (str == nullptr) return writeInt32(-1);
    status_t err = writeInt32(len);
    if (err == NO_ERROR) {
        auto *data = static_cast<uint8_t *>(writeInplace(len + sizeof(char)));
        if (data != nullptr) {
            memcpy(data, str, len);
            data[len] = 0;
            return NO_ERROR;
        }
        err = mError;
    }
    return err;
}

status_t Parcel::writeString16(const char16_t *str, size_t len) {
    if (str == nullptr) return writeInt32(-1);
    status_t err = writeInt32(len);
    if (err == NO_ERROR) {
        len *= sizeof(char16_t);
        auto *data = static_cast<uint8_t *>(writeInplace(len + sizeof(char16_t)));
        if (data != nullptr) {
            memcpy(data, str, len);
            const char16_t terminator = 0;
            memcpy(data + len, &terminator, sizeof(terminator));
            return NO_ERROR;
        }
        err = mError;
    }
    return err;
}

status_t Parcel::writeBlob(size_t len, void **outData) {
    if (len > INT32_MAX) return BAD_VALUE;
    // libbinder moves a blob above 16 KiB into ashmem where descriptors are allowed. Minato has no ashmem and keeps
    // every blob in place, so marshall() works on a parcel whose large blob Android's refuses.
    if (const status_t status = writeInt32(kBlobInplace); status != NO_ERROR) return status;
    void *ptr = writeInplace(len);
    if (ptr == nullptr) return NO_MEMORY;
    *outData = ptr;
    return NO_ERROR;
}

status_t Parcel::writeObject(const flat_binder_object &val, bool nullMetaData, const Object &object) {
    if (const status_t err = ensureCapacity(sizeof(val)); err != NO_ERROR) return err;
    memcpy(mData + mDataPos, &val, sizeof(val));
    if (val.hdr.type == BINDER_TYPE_FD) {
        // Fails before the object enters the list, as libbinder's.
        if (!mAllowFds) return FDS_NOT_ALLOWED;
        mHasFds = mFdsKnown = true;
    }
    if (nullMetaData || val.binder != 0) {
        Object entry = object;
        entry.offset = mDataPos;
        mObjects.push_back(entry);
    }
    return finishWrite(sizeof(val));
}

status_t Parcel::writeStrongBinder(JNIEnv *env, jobject binder) {
    // libbinder's flattenBinder for a local binder or none. A local binder's binder and cookie hold libbinder's
    // pointers; Minato's hold the entry's token instead, so the data never carries an address.
    flat_binder_object obj{};
    obj.hdr.type = BINDER_TYPE_BINDER;
    obj.flags = kSchedBits;
    Object object{0, 0, nullptr, -1};
    if (binder != nullptr) {
        object.binder = env->NewGlobalRef(binder);
        if (object.binder == nullptr) return NO_MEMORY;
        object.token = gNextToken++;
        obj.flags |= FLAT_BINDER_FLAG_ACCEPTS_FDS;
        obj.binder = object.token;
        obj.cookie = object.token;
    }
    if (const status_t status = writeObject(obj, false, object); status != NO_ERROR) {
        if (object.binder != nullptr) env->DeleteGlobalRef(object.binder);
        return status;
    }
    // finishFlattenBinder: the binder's stability.
    return writeInt32(binder != nullptr ? kStabilitySystem : kStabilityUndeclared);
}

status_t Parcel::writeDupFileDescriptor(int fd) {
    const int dupFd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (dupFd < 0) return -errno;
    flat_binder_object obj{};
    obj.hdr.type = BINDER_TYPE_FD;
    obj.flags = 0;
    obj.binder = 0;
    obj.handle = dupFd;
    // libbinder's cookie says the parcel owns the descriptor (1); every descriptor in a Minato parcel is its own, and
    // the cookie holds the entry's token.
    const Object object{0, gNextToken++, nullptr, dupFd};
    obj.cookie = object.token;
    const status_t err = writeObject(obj, true, object);
    if (err != NO_ERROR) {
        close(dupFd);
        return err;
    }
    fdTag(dupFd, nullptr, this);
    return NO_ERROR;
}

status_t Parcel::validateReadData(size_t upperBound) const {
    // Plain reads must not reach into an object. The list is checked in the order of its offsets, sorted (insertion
    // sort, as libbinder's) when it is not.
    if (!mObjectsSorted && mObjects.size() > 1) {
        bool sorted = true;
        for (size_t i = 1; i < mObjects.size() && sorted; i++) sorted = mObjects[i - 1].offset <= mObjects[i].offset;
        if (!sorted) {
            for (size_t i = 1; i < mObjects.size(); i++) {
                const Object temp = mObjects[i];
                size_t j = i;
                for (; j > 0 && mObjects[j - 1].offset > temp.offset; j--) mObjects[j] = mObjects[j - 1];
                mObjects[j] = temp;
            }
            mNextObjectHint = 0;
        }
        mObjectsSorted = true;
    }
    if (mNextObjectHint < mObjects.size() && upperBound > mObjects[mNextObjectHint].offset) {
        size_t nextObject = mNextObjectHint;
        do {
            if (mDataPos < mObjects[nextObject].offset + sizeof(flat_binder_object)) {
                ALOGE("Attempt to read from protected data in Parcel %p", this);
                return PERMISSION_DENIED;
            }
            nextObject++;
        } while (nextObject < mObjects.size() && upperBound > mObjects[nextObject].offset);
        mNextObjectHint = nextObject;
    }
    return NO_ERROR;
}

template <typename T>
status_t Parcel::readAligned(T *out) const {
    static_assert(sizeof(T) % 4 == 0);
    if (mDataPos + sizeof(T) <= mDataSize) {
        if (!mObjects.empty()) {
            if (const status_t err = validateReadData(mDataPos + sizeof(T)); err != NO_ERROR) {
                // The position still moves on by the value's size.
                mDataPos += sizeof(T);
                return err;
            }
        }
        memcpy(out, mData + mDataPos, sizeof(T));
        mDataPos += sizeof(T);
        return NO_ERROR;
    }
    return NOT_ENOUGH_DATA;
}

template <typename T>
T Parcel::readAligned() const {
    T result;
    if (readAligned(&result) != NO_ERROR) result = 0;
    return result;
}

int32_t Parcel::readInt32() const {
    return readAligned<int32_t>();
}

status_t Parcel::readInt32(int32_t *out) const {
    return readAligned(out);
}

int64_t Parcel::readInt64() const {
    return readAligned<int64_t>();
}

float Parcel::readFloat() const {
    return readAligned<float>();
}

double Parcel::readDouble() const {
    return readAligned<double>();
}

status_t Parcel::readFloat(float *out) const {
    return readAligned(out);
}

bool Parcel::readBool() const {
    return readInt32() != 0;
}

int8_t Parcel::readByte() const {
    return int8_t(readInt32());
}

uint32_t Parcel::readUint32() const {
    return readAligned<uint32_t>();
}

// As libbinder's readData for a vector of bytes: a negative length is a null vector, one beyond the data is refused.
status_t Parcel::readByteVector(std::vector<uint8_t> *val) const {
    int32_t size;
    if (const status_t status = readInt32(&size); status != NO_ERROR) return status;
    if (size < 0) return UNEXPECTED_NULL;
    if (static_cast<size_t>(size) > dataAvail()) return BAD_VALUE;
    val->clear();
    const void *data = readInplace(static_cast<size_t>(size));
    if (data == nullptr) return BAD_VALUE;
    val->resize(static_cast<size_t>(size));
    if (size > 0) memcpy(val->data(), data, static_cast<size_t>(size));
    return NO_ERROR;
}

const void *Parcel::readInplace(size_t len) const {
    if (len > INT32_MAX) return nullptr;
    const size_t padded = padSize(len);
    if (mDataPos + padded >= mDataPos && mDataPos + padded <= mDataSize && len <= padded) {
        if (!mObjects.empty()) {
            if (validateReadData(mDataPos + padded) != NO_ERROR) {
                mDataPos += padded;
                return nullptr;
            }
        }
        const void *data = mData + mDataPos;
        mDataPos += padded;
        return data;
    }
    return nullptr;
}

const char *Parcel::readString8Inplace(size_t *outLen) const {
    const int32_t size = readInt32();
    if (size >= 0 && size < INT32_MAX) {
        *outLen = size;
        const auto *str = static_cast<const char *>(readInplace(size + 1));
        // A string without its terminator is none (libbinder also logs SafetyNet's 172655291).
        if (str != nullptr && str[size] == '\0') return str;
    }
    *outLen = 0;
    return nullptr;
}

const char16_t *Parcel::readString16Inplace(size_t *outLen) const {
    const int32_t size = readInt32();
    if (size >= 0 && size < INT32_MAX) {
        *outLen = size;
        const auto *str = static_cast<const char16_t *>(readInplace((size + 1) * sizeof(char16_t)));
        if (str != nullptr) {
            char16_t terminator;
            memcpy(&terminator, str + size, sizeof(terminator));
            if (terminator == u'\0') return str;
        }
    }
    *outLen = 0;
    return nullptr;
}

status_t Parcel::readBlob(size_t len, const void **outData) const {
    int32_t blobType;
    if (const status_t status = readInt32(&blobType); status != NO_ERROR) return status;
    if (blobType == kBlobInplace) {
        const void *ptr = readInplace(len);
        if (ptr == nullptr) return BAD_VALUE;
        *outData = ptr;
        return NO_ERROR;
    }
    // An ashmem blob's descriptor, which in Minato is never an ashmem region: libbinder answers BAD_VALUE then too.
    readFileDescriptor();
    return BAD_VALUE;
}

bool Parcel::readObject(bool nullMetaData, flat_binder_object *out, const Object **entry) const {
    *entry = nullptr;
    const size_t dpos = mDataPos;
    if (dpos + sizeof(flat_binder_object) > mDataSize) return false;
    memcpy(out, mData + dpos, sizeof(flat_binder_object));
    mDataPos = dpos + sizeof(flat_binder_object);
    // A null binder is written without an entry.
    if (!nullMetaData && out->cookie == 0 && out->binder == 0) return true;

    // libbinder's search from the hint, forwards and then back.
    const size_t n = mObjects.size();
    if (n > 0) {
        size_t opos = mNextObjectHint;
        if (opos < n) {
            while (opos < n - 1 && mObjects[opos].offset < dpos) opos++;
        } else {
            opos = n - 1;
        }
        if (mObjects[opos].offset != dpos) {
            while (opos > 0 && mObjects[opos].offset > dpos) opos--;
        }
        if (mObjects[opos].offset == dpos) {
            mNextObjectHint = opos + 1;
            // The object is the entry's whose token the cookie carries; bytes written over an object stand for
            // none, where libbinder would take their cookie for a pointer.
            for (const Object &object : mObjects) {
                if (object.offset == dpos && object.token == out->cookie) {
                    *entry = &object;
                    break;
                }
            }
            return true;
        }
    }
    ALOGW("Attempt to read object from Parcel %p at offset %zu that is not in the object list", this, dpos);
    return false;
}

jobject Parcel::readStrongBinder(JNIEnv *env) const {
    flat_binder_object flat;
    const Object *entry;
    if (!readObject(false, &flat, &entry)) return nullptr;
    jobject binder = nullptr;
    switch (flat.hdr.type) {
        case BINDER_TYPE_BINDER:
            if (flat.cookie != 0) {
                if (entry == nullptr || entry->binder == nullptr) return nullptr;
                binder = entry->binder;
            }
            break;
        case BINDER_TYPE_HANDLE:
            // A remote binder, which no parcel of Minato's process can hold: none (libbinder makes a proxy).
            break;
        default:
            return nullptr;
    }
    // finishUnflattenBinder: the stability must be the binder's, and a null binder's undeclared.
    int32_t stability;
    if (readInt32(&stability) != NO_ERROR) return nullptr;
    if (binder == nullptr) {
        if (stability != kStabilityUndeclared) ALOGE("Null binder written with stability %d.", stability);
        return nullptr;
    }
    if (stability != kStabilitySystem) {
        ALOGE("Interface being set with stability %d but it is already marked as system stability", stability);
        return nullptr;
    }
    return env->NewLocalRef(binder);
}

int Parcel::readFileDescriptor() const {
    flat_binder_object flat;
    const Object *entry;
    if (readObject(true, &flat, &entry) && flat.hdr.type == BINDER_TYPE_FD) {
        // The entry's descriptor, not the handle's bytes.
        if (entry != nullptr && entry->binder == nullptr) return entry->fd;
    }
    return BAD_TYPE;
}

status_t Parcel::appendFrom(const Parcel *parcel, size_t offset, size_t len) {
    if (len == 0) return NO_ERROR;
    if (len > INT32_MAX) return BAD_VALUE;
    if (offset > parcel->mDataSize || len > parcel->mDataSize || offset + len > parcel->mDataSize) return BAD_VALUE;

    const size_t startPos = mDataPos;
    // libbinder makes room for mDataSize + len but copies at the position; Minato makes room for both.
    if (mDataSize + len > mDataCapacity || mDataPos + len > mDataCapacity) {
        if (const status_t err = growData(len); err != NO_ERROR) return err;
        if (mDataSize + len > mDataCapacity || mDataPos + len > mDataCapacity) return NO_MEMORY;
    }
    // The source is read after growing, so a parcel appended to itself is read from its new buffer.
    memmove(mData + mDataPos, parcel->mData + offset, len);
    mDataPos += len;
    mDataSize += len;

    // The source's objects that lie wholly in the range, each with a new entry: a new global reference to a binder
    // and a dup of a descriptor, which this parcel then owns. (libbinder takes the list's run from the first to the
    // last such object, which equals these when the list is sorted.)
    std::vector<Object> sources;
    for (const Object &object : parcel->mObjects) {
        if (object.offset >= offset && object.offset + sizeof(flat_binder_object) <= offset + len) {
            sources.push_back(object);
        }
    }
    status_t err = NO_ERROR;
    JNIEnv *env = sources.empty() ? nullptr : currentEnv();
    for (const Object &source : sources) {
        const size_t at = source.offset - offset + startPos;
        flat_binder_object flat;
        memcpy(&flat, mData + at, sizeof(flat));
        // Only an object whose bytes are still its own: bytes written over it since stay plain data.
        const uint32_t type = source.binder != nullptr ? BINDER_TYPE_BINDER : BINDER_TYPE_FD;
        if (flat.hdr.type != type || flat.cookie != source.token) continue;
        Object copy{at, gNextToken++, nullptr, -1};
        if (source.binder != nullptr) {
            copy.binder = env != nullptr ? env->NewGlobalRef(source.binder) : nullptr;
            if (copy.binder == nullptr) {
                err = NO_MEMORY;
                continue;
            }
            flat.binder = copy.token;
        } else {
            copy.fd = fcntl(source.fd, F_DUPFD_CLOEXEC, 0);
            if (copy.fd >= 0) fdTag(copy.fd, nullptr, this);
            flat.binder = 0;
            flat.handle = copy.fd;
            mHasFds = mFdsKnown = true;
            if (!mAllowFds) err = FDS_NOT_ALLOWED;
        }
        flat.cookie = copy.token;
        memcpy(mData + copy.offset, &flat, sizeof(flat));
        mObjects.push_back(copy);
    }
    return err;
}

int Parcel::compareData(const Parcel &other) const {
    const size_t size = dataSize();
    if (size != other.dataSize()) return size < other.dataSize() ? -1 : 1;
    // libbinder compares dataSize() bytes, which a position beyond the data takes past its buffer; Minato compares
    // what both buffers hold.
    const size_t n = std::min({size, mDataCapacity, other.mDataCapacity});
    return n == 0 ? 0 : memcmp(mData, other.mData, n);
}

status_t Parcel::compareDataInRange(size_t thisOffset, const Parcel &other, size_t otherOffset, size_t len,
                                    int *result) const {
    if (len > INT32_MAX || thisOffset > INT32_MAX || otherOffset > INT32_MAX) return BAD_VALUE;
    size_t thisLimit;
    if (__builtin_add_overflow(thisOffset, len, &thisLimit) || thisLimit > mDataSize) return BAD_VALUE;
    size_t otherLimit;
    if (__builtin_add_overflow(otherOffset, len, &otherLimit) || otherLimit > other.mDataSize) return BAD_VALUE;
    *result = len == 0 ? 0 : memcmp(mData + thisOffset, other.mData + otherOffset, len);
    return NO_ERROR;
}

status_t Parcel::hasBindersInRange(size_t offset, size_t len, bool *result) const {
    if (len > INT32_MAX || offset > INT32_MAX) return BAD_VALUE;
    size_t limit;
    if (__builtin_add_overflow(offset, len, &limit) || limit > mDataSize) return BAD_VALUE;
    *result = false;
    for (const Object &object : mObjects) {
        const size_t pos = object.offset;
        if (pos < offset) continue;
        if (pos + sizeof(flat_binder_object) > offset + len) {
            if (mObjectsSorted) break;
            continue;
        }
        uint32_t type;
        memcpy(&type, mData + pos, sizeof(type));
        if (type == BINDER_TYPE_BINDER || type == BINDER_TYPE_HANDLE) {
            *result = true;
            break;
        }
    }
    return NO_ERROR;
}

status_t Parcel::hasFileDescriptorsInRange(size_t offset, size_t len, bool *result) const {
    if (len > INT32_MAX || offset > INT32_MAX) return BAD_VALUE;
    size_t limit;
    if (__builtin_add_overflow(offset, len, &limit) || limit > mDataSize) return BAD_VALUE;
    *result = false;
    for (const Object &object : mObjects) {
        const size_t pos = object.offset;
        if (pos < offset) continue;
        if (pos + sizeof(flat_binder_object) > offset + len) {
            if (mObjectsSorted) break;
            continue;
        }
        uint32_t type;
        memcpy(&type, mData + pos, sizeof(type));
        if (type == BINDER_TYPE_FD) {
            *result = true;
            break;
        }
    }
    return NO_ERROR;
}

bool Parcel::hasFileDescriptors() const {
    if (!mFdsKnown) scanForFds();
    return mHasFds;
}

status_t Parcel::hasBinders(bool *result) const {
    const status_t status = hasBindersInRange(0, dataSize(), result);
    ALOGE_IF(status != NO_ERROR, "Error %d calling hasBindersInRange()", status);
    return status;
}

void Parcel::scanForFds() const {
    bool hasFds = mHasFds;
    const status_t status = hasFileDescriptorsInRange(0, dataSize(), &hasFds);
    ALOGE_IF(status != NO_ERROR, "Error %d calling hasFileDescriptorsInRange()", status);
    mHasFds = hasFds;
    mFdsKnown = true;
}

void Parcel::updateWorkSourceRequestHeaderPosition() const {
    // Only the first header written or read counts.
    if (!mRequestHeaderPresent) {
        mWorkSourceRequestHeaderPosition = dataPosition();
        mRequestHeaderPresent = true;
    }
}

status_t Parcel::writeInterfaceToken(const char16_t *str, size_t len, int32_t strictModePolicy) {
    writeInt32(strictModePolicy | kStrictModePenaltyGather);
    updateWorkSourceRequestHeaderPosition();
    writeInt32(kUnsetWorkSource);
    writeInt32(kHeader);
    return writeString16(str, len);
}

bool Parcel::enforceInterface(const char16_t *interface, size_t len, int32_t *strictModePolicy) const {
    *strictModePolicy = readInt32();
    updateWorkSourceRequestHeaderPosition();
    readInt32();  // the work source, which no call carries in from another process
    const int32_t header = readInt32();
    if (header != kHeader) {
        ALOGE("Expecting header 0x%x but found 0x%x. Mixing copies of libbinder?", kHeader, header);
        return false;
    }
    size_t parcelInterfaceLen;
    const char16_t *parcelInterface = readString16Inplace(&parcelInterfaceLen);
    if (len == parcelInterfaceLen &&
        (len == 0 || memcmp(parcelInterface, interface, len * sizeof(char16_t)) == 0)) {
        return true;
    }
    ALOGW("**** enforceInterface() expected '%s' but read '%s'", String8(interface, len).c_str(),
          String8(parcelInterface, parcelInterfaceLen).c_str());
    return false;
}

int32_t Parcel::readCallingWorkSourceUid() const {
    if (!mRequestHeaderPresent) return kUnsetWorkSource;
    const size_t initialPosition = dataPosition();
    setDataPosition(mWorkSourceRequestHeaderPosition);
    const int32_t uid = readInt32();
    setDataPosition(initialPosition);
    return uid;
}

bool Parcel::replaceCallingWorkSourceUid(int32_t uid) {
    if (!mRequestHeaderPresent) return false;
    const size_t initialPosition = dataPosition();
    setDataPosition(mWorkSourceRequestHeaderPosition);
    const status_t err = writeInt32(uid);
    setDataPosition(initialPosition);
    return err == NO_ERROR;
}

}  // namespace android
