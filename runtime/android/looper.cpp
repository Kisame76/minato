// ALooper of libandroid.so (ADR-0028, decision 9), written for Minato from the NDK's documentation and
// Android 15's answers (fixtures/suite/looper-test.c). A thread's looper waits in an epoll for its
// descriptors and for an eventfd that ALooper_wake raises. A descriptor has either an ident, which
// pollOnce returns, or a callback, which pollOnce calls and which removes it by answering 0. Each
// registration has a sequence number of its own, so an event of an old registration never reaches a
// new one of the same descriptor. A descriptor closed before its removal leaves the epoll with an
// entry nobody can remove, so the looper builds a new epoll at its next wait. Errors go to the log
// with Android's words.
#include <android/log.h>
#include <android/looper.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

namespace {

constexpr uint64_t kWakeSequence = 1;  // the eventfd's; descriptors get the numbers after it
constexpr int kMaxEvents = 16;         // per epoll_wait, as Android's

struct Request {
    int fd;
    int ident;  // ALOOPER_POLL_CALLBACK for a callback
    int events;
    ALooper_callbackFunc callback;
    void* data;
    uint64_t sequence;
};

struct Response {
    Request request;
    int events;
};

}  // namespace

struct ALooper {
    uint32_t references;  // atomic
    bool allow_non_callbacks;
    int wake_fd;
    pthread_mutex_t lock;
    // Under the lock; epoll_fd changes only on the looper's own thread.
    int epoll_fd;
    bool rebuild;
    uint64_t next_sequence;
    Request* requests;
    size_t request_count;
    size_t request_capacity;
    // The looper's own thread alone.
    Response responses[kMaxEvents];
    size_t response_count;
    size_t response_index;
    uint64_t polls;  // a callback that polls again replaces the responses
};

namespace {

#define LOOPER_LOG(priority, ...) __android_log_print(ANDROID_LOG_##priority, "Looper", __VA_ARGS__)

[[noreturn]] void fatal(const char* what) {
    __android_log_assert(nullptr, "Looper", "%s: %s", what, strerror(errno));
}

uint32_t epoll_events(int events) {
    uint32_t result = 0;
    if (events & ALOOPER_EVENT_INPUT) result |= EPOLLIN;
    if (events & ALOOPER_EVENT_OUTPUT) result |= EPOLLOUT;
    return result;
}

epoll_event epoll_item(uint32_t events, uint64_t sequence) {
    epoll_event item{};
    item.events = events;
    item.data.u64 = sequence;
    return item;
}

void wake(ALooper* looper) {
    const uint64_t one = 1;
    ssize_t written;
    do {
        written = write(looper->wake_fd, &one, sizeof one);
    } while (written < 0 && errno == EINTR);
    if (written != static_cast<ssize_t>(sizeof one) && errno != EAGAIN) {
        __android_log_assert(nullptr, "Looper", "Could not write wake signal to fd %d (returned %zd): %s",
                             looper->wake_fd, written, strerror(errno));
    }
}

void awoken(ALooper* looper) {
    uint64_t counter;
    while (read(looper->wake_fd, &counter, sizeof counter) < 0 && errno == EINTR) {
    }
}

void rebuild_locked(ALooper* looper) {
    if (looper->epoll_fd >= 0) close(looper->epoll_fd);
    looper->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (looper->epoll_fd < 0) fatal("Could not create epoll instance");
    epoll_event wake_item = epoll_item(EPOLLIN, kWakeSequence);
    if (epoll_ctl(looper->epoll_fd, EPOLL_CTL_ADD, looper->wake_fd, &wake_item) != 0) {
        fatal("Could not add wake event fd to epoll instance");
    }
    for (size_t i = 0; i < looper->request_count; ++i) {
        const Request& request = looper->requests[i];
        epoll_event item = epoll_item(epoll_events(request.events), request.sequence);
        if (epoll_ctl(looper->epoll_fd, EPOLL_CTL_ADD, request.fd, &item) != 0) {
            LOOPER_LOG(ERROR, "Error adding epoll events for fd %d while rebuilding epoll set: %s", request.fd,
                       strerror(errno));
        }
    }
}

void schedule_rebuild_locked(ALooper* looper) {
    if (!looper->rebuild) {
        looper->rebuild = true;
        wake(looper);
    }
}

Request* find_fd_locked(ALooper* looper, int fd) {
    for (size_t i = 0; i < looper->request_count; ++i) {
        if (looper->requests[i].fd == fd) return &looper->requests[i];
    }
    return nullptr;
}

Request* find_sequence_locked(ALooper* looper, uint64_t sequence) {
    for (size_t i = 0; i < looper->request_count; ++i) {
        if (looper->requests[i].sequence == sequence) return &looper->requests[i];
    }
    return nullptr;
}

// Room for one more registration.
bool reserve_locked(ALooper* looper) {
    if (looper->request_count < looper->request_capacity) return true;
    const size_t capacity = looper->request_capacity == 0 ? 8 : looper->request_capacity * 2;
    auto* grown = static_cast<Request*>(realloc(looper->requests, capacity * sizeof(Request)));
    if (grown == nullptr) return false;
    looper->requests = grown;
    looper->request_capacity = capacity;
    return true;
}

// 1 when the registration was removed, 0 when there was none, -1 on an error.
int remove_sequence_locked(ALooper* looper, uint64_t sequence) {
    Request* request = find_sequence_locked(looper, sequence);
    if (request == nullptr) return 0;
    const int fd = request->fd;
    *request = looper->requests[--looper->request_count];
    if (epoll_ctl(looper->epoll_fd, EPOLL_CTL_DEL, fd, nullptr) != 0) {
        // EBADF or ENOENT: the descriptor was closed before it was removed, as a callback may do
        // before it answers 0. Its entry may still be in the epoll, under an open file only a
        // new epoll forgets.
        const bool closed = errno == EBADF || errno == ENOENT;
        if (!closed) LOOPER_LOG(ERROR, "Error removing epoll events for fd %d: %s", fd, strerror(errno));
        schedule_rebuild_locked(looper);
        if (!closed) return -1;
    }
    return 1;
}

pthread_key_t thread_key;
pthread_once_t thread_key_once = PTHREAD_ONCE_INIT;

void release_thread_looper(void* looper) {
    ALooper_release(static_cast<ALooper*>(looper));
}

void make_thread_key() {
    errno = pthread_key_create(&thread_key, release_thread_looper);
    if (errno != 0) fatal("Could not allocate TLS key");
}

ALooper* thread_looper() {
    pthread_once(&thread_key_once, make_thread_key);
    return static_cast<ALooper*>(pthread_getspecific(thread_key));
}

// Collects what the epoll reported, under the lock; returns the result without callbacks.
int collect_locked(ALooper* looper, int count, const epoll_event* items) {
    if (looper->rebuild) {
        looper->rebuild = false;
        rebuild_locked(looper);
        return ALOOPER_POLL_WAKE;
    }
    if (count < 0) {
        if (errno == EINTR) return ALOOPER_POLL_WAKE;
        LOOPER_LOG(WARN, "Poll failed with an unexpected error: %s", strerror(errno));
        return ALOOPER_POLL_ERROR;
    }
    if (count == 0) return ALOOPER_POLL_TIMEOUT;
    for (int i = 0; i < count; ++i) {
        const uint64_t sequence = items[i].data.u64;
        const uint32_t events = items[i].events;
        if (sequence == kWakeSequence) {
            if (events & EPOLLIN) {
                awoken(looper);
            } else {
                LOOPER_LOG(WARN, "Ignoring unexpected epoll events 0x%x on wake event fd.", events);
            }
            continue;
        }
        const Request* request = find_sequence_locked(looper, sequence);
        if (request == nullptr) {
            LOOPER_LOG(WARN, "Ignoring unexpected epoll events 0x%x for sequence number %" PRIu64
                       " that is no longer registered.", events, sequence);
            continue;
        }
        int looper_events = 0;
        if (events & EPOLLIN) looper_events |= ALOOPER_EVENT_INPUT;
        if (events & EPOLLOUT) looper_events |= ALOOPER_EVENT_OUTPUT;
        if (events & EPOLLERR) looper_events |= ALOOPER_EVENT_ERROR;
        if (events & EPOLLHUP) looper_events |= ALOOPER_EVENT_HANGUP;
        looper->responses[looper->response_count++] = {*request, looper_events};
    }
    return ALOOPER_POLL_WAKE;
}

// One wait: the responses of the descriptors that are ready, whose callbacks have run.
int poll_inner(ALooper* looper, int timeout) {
    const uint64_t poll = ++looper->polls;
    looper->response_count = 0;
    looper->response_index = 0;
    epoll_event items[kMaxEvents];
    const int count = epoll_wait(looper->epoll_fd, items, kMaxEvents, timeout);
    pthread_mutex_lock(&looper->lock);
    int result = collect_locked(looper, count, items);
    pthread_mutex_unlock(&looper->lock);
    for (size_t i = 0; i < looper->response_count && looper->polls == poll; ++i) {
        const Response response = looper->responses[i];
        if (response.request.ident != ALOOPER_POLL_CALLBACK) continue;
        if (response.request.callback(response.request.fd, response.events, response.request.data) == 0) {
            pthread_mutex_lock(&looper->lock);
            remove_sequence_locked(looper, response.request.sequence);
            pthread_mutex_unlock(&looper->lock);
        }
        result = ALOOPER_POLL_CALLBACK;
    }
    return result;
}

int poll_once(ALooper* looper, int timeout, int* out_fd, int* out_events, void** out_data) {
    int result = 0;
    for (;;) {
        while (looper->response_index < looper->response_count) {
            const Response& response = looper->responses[looper->response_index++];
            if (response.request.ident >= 0) {
                if (out_fd != nullptr) *out_fd = response.request.fd;
                if (out_events != nullptr) *out_events = response.events;
                if (out_data != nullptr) *out_data = response.request.data;
                return response.request.ident;
            }
        }
        if (result != 0) {
            if (out_fd != nullptr) *out_fd = 0;
            if (out_events != nullptr) *out_events = 0;
            if (out_data != nullptr) *out_data = nullptr;
            return result;
        }
        result = poll_inner(looper, timeout);
    }
}

int64_t now_nanoseconds() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1'000'000'000LL + now.tv_nsec;
}

}  // namespace

extern "C" {

ALooper* ALooper_forThread() {
    return thread_looper();
}

ALooper* ALooper_prepare(int opts) {
    const bool allow_non_callbacks = (opts & ALOOPER_PREPARE_ALLOW_NON_CALLBACKS) != 0;
    ALooper* looper = thread_looper();
    if (looper == nullptr) {
        looper = static_cast<ALooper*>(calloc(1, sizeof(ALooper)));
        if (looper == nullptr) return nullptr;
        looper->references = 1;  // the thread's, released when the thread ends
        looper->allow_non_callbacks = allow_non_callbacks;
        looper->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (looper->wake_fd < 0) fatal("Could not make wake event fd");
        pthread_mutex_init(&looper->lock, nullptr);
        looper->epoll_fd = -1;
        looper->next_sequence = kWakeSequence + 1;
        rebuild_locked(looper);
        pthread_setspecific(thread_key, looper);
    }
    if (looper->allow_non_callbacks != allow_non_callbacks) {
        LOOPER_LOG(WARN, "Looper already prepared for this thread with a different value for the "
                   "LOOPER_PREPARE_ALLOW_NON_CALLBACKS option.");
    }
    return looper;
}

void ALooper_acquire(ALooper* looper) {
    __atomic_add_fetch(&looper->references, 1, __ATOMIC_RELAXED);
}

void ALooper_release(ALooper* looper) {
    if (__atomic_sub_fetch(&looper->references, 1, __ATOMIC_ACQ_REL) != 0) return;
    close(looper->epoll_fd);
    close(looper->wake_fd);
    pthread_mutex_destroy(&looper->lock);
    free(looper->requests);
    free(looper);
}

int ALooper_pollOnce(int timeoutMillis, int* outFd, int* outEvents, void** outData) {
    ALooper* looper = thread_looper();
    if (looper == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, "ALooper", "ALooper_pollOnce: No looper for this thread!");
        return ALOOPER_POLL_ERROR;
    }
    return poll_once(looper, timeoutMillis, outFd, outEvents, outData);
}

// Hidden by the NDK's headers since API 35, still there for the apps built before.
int ALooper_pollAll(int timeoutMillis, int* outFd, int* outEvents, void** outData) {
    ALooper* looper = thread_looper();
    if (looper == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, "ALooper", "ALooper_pollAll: No looper for this thread!");
        return ALOOPER_POLL_ERROR;
    }
    if (timeoutMillis <= 0) {
        int result;
        do {
            result = poll_once(looper, timeoutMillis, outFd, outEvents, outData);
        } while (result == ALOOPER_POLL_CALLBACK);
        return result;
    }
    const int64_t end = now_nanoseconds() + timeoutMillis * 1'000'000LL;
    for (;;) {
        const int result = poll_once(looper, timeoutMillis, outFd, outEvents, outData);
        if (result != ALOOPER_POLL_CALLBACK) return result;
        const int64_t left = end - now_nanoseconds();
        if (left <= 0) return ALOOPER_POLL_TIMEOUT;
        timeoutMillis = static_cast<int>((left + 999'999) / 1'000'000);  // rounded up
    }
}

void ALooper_wake(ALooper* looper) {
    wake(looper);
}

int ALooper_addFd(ALooper* looper, int fd, int ident, int events, ALooper_callbackFunc callback, void* data) {
    if (callback == nullptr) {
        if (!looper->allow_non_callbacks) {
            LOOPER_LOG(ERROR, "Invalid attempt to set NULL callback but not allowed for this looper.");
            return -1;
        }
        if (ident < 0) {
            LOOPER_LOG(ERROR, "Invalid attempt to set NULL callback with ident < 0.");
            return -1;
        }
    } else {
        ident = ALOOPER_POLL_CALLBACK;
    }
    pthread_mutex_lock(&looper->lock);
    const uint64_t sequence = looper->next_sequence++;
    const Request request{fd, ident, events, callback, data, sequence};
    epoll_event item = epoll_item(epoll_events(events), sequence);
    int result = 1;
    if (Request* existing = find_fd_locked(looper, fd); existing != nullptr) {
        if (epoll_ctl(looper->epoll_fd, EPOLL_CTL_MOD, fd, &item) != 0) {
            // ENOENT: an older descriptor of this number was closed before it was removed, and this
            // one is new to the epoll.
            if (errno != ENOENT) {
                LOOPER_LOG(ERROR, "Error modifying epoll events for fd %d: %s", fd, strerror(errno));
                result = -1;
            } else if (epoll_ctl(looper->epoll_fd, EPOLL_CTL_ADD, fd, &item) != 0) {
                LOOPER_LOG(ERROR, "Error modifying or adding epoll events for fd %d: %s", fd, strerror(errno));
                result = -1;
            } else {
                schedule_rebuild_locked(looper);
            }
        }
        if (result == 1) *existing = request;
    } else if (!reserve_locked(looper)) {
        LOOPER_LOG(ERROR, "Error adding epoll events for fd %d: %s", fd, strerror(ENOMEM));
        result = -1;
    } else if (epoll_ctl(looper->epoll_fd, EPOLL_CTL_ADD, fd, &item) != 0) {
        LOOPER_LOG(ERROR, "Error adding epoll events for fd %d: %s", fd, strerror(errno));
        result = -1;
    } else {
        looper->requests[looper->request_count++] = request;
    }
    pthread_mutex_unlock(&looper->lock);
    return result;
}

int ALooper_removeFd(ALooper* looper, int fd) {
    pthread_mutex_lock(&looper->lock);
    const Request* request = find_fd_locked(looper, fd);
    const int result = request == nullptr ? 0 : remove_sequence_locked(looper, request->sequence);
    pthread_mutex_unlock(&looper->lock);
    return result;
}

}  // extern "C"
