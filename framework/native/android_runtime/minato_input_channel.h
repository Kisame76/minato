// Ours (ADR-0045 step 7b2): what a Java InputChannel holds on Minato, shared by InputChannel's natives
// (android_view_InputChannel.cpp) and InputEventReceiver's (android_view_InputEventReceiver.cpp). Where AOSP's holds
// libinput's InputChannel, its end of a socket pair that system_server's dispatcher writes, ours holds the guest
// descriptor of the session's input channel (guest_interface.h's input_connect), which the host's touches fill.
#pragma once

#include <jni.h>

#include <memory>
#include <string>

namespace android {

// A connected input channel's descriptor, closed with its last owner: its InputChannels until their dispose, and each
// receiver that watches it, as AOSP's shared InputChannel closes its socket.
class InputDescriptor {
public:
    explicit InputDescriptor(int fd) : mFd(fd) {}
    ~InputDescriptor();
    InputDescriptor(const InputDescriptor &) = delete;
    InputDescriptor &operator=(const InputDescriptor &) = delete;

    int get() const { return mFd; }

private:
    const int mFd;
};

// What InputChannel.mPtr points to, as AOSP's NativeInputChannel: the descriptor, null once disposed, and the name.
struct NativeInputChannel {
    std::shared_ptr<InputDescriptor> descriptor;
    std::string name;
};

// The descriptor of a Java InputChannel, or null for one without (never set, or disposed).
std::shared_ptr<InputDescriptor> android_view_InputChannel_getDescriptor(JNIEnv *env, jobject inputChannelObj);

// A Java InputChannel's name, "uninitialized" for one without a native channel, as InputChannel.getName.
std::string android_view_InputChannel_getName(JNIEnv *env, jobject inputChannelObj);

// A new Java InputChannel on guest descriptor `fd`, named `name`, as AOSP's android_view_InputChannel_createJavaObject
// makes one; null, with `fd` closed, when it cannot be made.
jobject android_view_InputChannel_createJavaObject(JNIEnv *env, int fd, const std::string &name);

}  // namespace android
