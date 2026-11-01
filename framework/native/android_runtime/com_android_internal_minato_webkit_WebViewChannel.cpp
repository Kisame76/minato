// The natives of com.android.internal.minato.webkit.WebViewChannel (ADR-0056 step b2): a WebView's requests to the
// host (APKRT_REQUEST_WEBVIEW's MAKE, PLACE, REMOVE, CALL and ANSWER) and the reading of its record channel, whole
// records joined by the runtime's header-only WebViewReader, on the provider's pump thread.

#include <errno.h>
#include <jni.h>
#include <nativehelper/JNIHelp.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <vector>

#include <APKRuntime/apkrt_webview_reader.h>
#include <APKRuntime/guest_interface.h>

#include "core_jni_helpers.h"

namespace android {

namespace {

/// APKRT_REQUEST_WEBVIEW with its struct apkrt_webview_request: `frame` is PLACE's frame and clip (eight ints, left,
/// top, right, bottom of each) or null, `payload` a CALL's or an ANSWER's bytes or null. MAKE returns the channel's
/// fd; the others 0; each a negative errno on a refusal.
jint nativeRequest(JNIEnv *env, jclass, jint op, jlong token, jlong requestId, jlong window, jintArray frame,
                   jbyteArray payload) {
    apkrt_webview_request request{};
    request.op = static_cast<uint32_t>(op);
    request.token = static_cast<uint64_t>(token);
    request.request_id = static_cast<uint64_t>(requestId);
    request.window = static_cast<uint64_t>(window);
    request.fd = -1;
    if (frame != nullptr) {
        if (env->GetArrayLength(frame) != 8) return -EINVAL;
        jint edges[8];
        env->GetIntArrayRegion(frame, 0, 8, edges);
        request.left = edges[0];
        request.top = edges[1];
        request.right = edges[2];
        request.bottom = edges[3];
        request.clip_left = edges[4];
        request.clip_top = edges[5];
        request.clip_right = edges[6];
        request.clip_bottom = edges[7];
    }
    std::vector<jbyte> bytes;
    if (payload != nullptr) {
        bytes.resize(static_cast<size_t>(env->GetArrayLength(payload)));
        env->GetByteArrayRegion(payload, 0, static_cast<jsize>(bytes.size()), bytes.data());
        request.payload = reinterpret_cast<uintptr_t>(bytes.data());
        request.payload_length = static_cast<uint32_t>(bytes.size());
    }
    const long result = syscall(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_WEBVIEW,
                                reinterpret_cast<uintptr_t>(&request), 0);
    return result < 0 ? -errno : static_cast<jint>(result);
}

/// The next whole record of the channel `fd`, blocking: its payload, with `meta` set to {1, type, request_id}; null with
/// meta[0] 0 at the channel's end or a negative errno on an error.
jbyteArray nativeNext(JNIEnv *env, jclass, jint fd, jlongArray meta) {
    apkrt::channels::WebViewReader reader(fd);
    apkrt::channels::WebViewRecord record;
    const long status = reader.next(record);
    jlong values[3] = {status, 0, 0};
    if (status != 1) {
        env->SetLongArrayRegion(meta, 0, 3, values);
        return nullptr;
    }
    values[1] = static_cast<jlong>(record.type);
    values[2] = static_cast<jlong>(record.request_id);
    env->SetLongArrayRegion(meta, 0, 3, values);
    jbyteArray bytes = env->NewByteArray(static_cast<jsize>(record.payload.size()));
    if (bytes == nullptr) return nullptr;
    env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(record.payload.size()),
                            reinterpret_cast<const jbyte *>(record.payload.data()));
    return bytes;
}

/// Closes the guest's end of a channel, which the provider does once its pump has met the end.
void nativeClose(JNIEnv *, jclass, jint fd) {
    close(fd);
}

const JNINativeMethod gMethods[] = {
        {"nativeRequest", "(IJJJ[I[B)I", reinterpret_cast<void *>(nativeRequest)},
        {"nativeNext", "(I[J)[B", reinterpret_cast<void *>(nativeNext)},
        {"nativeClose", "(I)V", reinterpret_cast<void *>(nativeClose)},
};

}  // namespace

int register_com_android_internal_minato_webkit_WebViewChannel(JNIEnv *env) {
    return RegisterMethodsOrDie(env, "com/android/internal/minato/webkit/WebViewChannel", gMethods, NELEM(gMethods));
}

}  // namespace android
