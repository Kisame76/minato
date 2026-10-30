// Checks the WebView request dispatch (ADR-0056 step a): APKRT_REQUEST_WEBVIEW with a struct apkrt_webview_request is
// checked and handed to the host's webview handler as an apkrt_webview_call with host-safe fields. MAKE opens the
// record channel and returns its fd; a duplicate token is EEXIST and the 17th view ENOSPC; PLACE, CALL and ANSWER
// reach the handler (CALL/ANSWER carry a payload the runtime copied, or an ANSWER_FD the runtime dup'd); REMOVE closes
// the channel so the guest's reader meets EOF; a bad address, op, token or field is refused. A process has one session,
// so one begin, lazily, with a handler that records the calls. The test's thread plays the guest and calls the
// dispatcher directly, as session_test.cpp does; each test uses its own tokens so the live set does not cross them.
#include "descriptors.h"
#include "syscall_frame.h"

#include "APKRuntime/apkrt.h"
#include "APKRuntime/apkrt_webview_reader.h"
#include "check.h"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

using apkrt::channels::WebViewReader;
using apkrt::channels::WebViewRecord;

constexpr long enoent = 2, ebadf = 9, efault = 14, eexist = 17, einval = 22, enospc = 28;

template <typename T>
std::uint64_t address(T *pointer) {
    return reinterpret_cast<std::uint64_t>(pointer);
}

/// A syscall of the calling host thread, straight into the dispatcher, as session_test.cpp does.
long call(std::uint64_t number, std::uint64_t a0 = 0, std::uint64_t a1 = 0) {
    apkrt_syscall_frame frame{};
    frame.x[0] = a0;
    frame.x[1] = a1;
    frame.x[8] = number;
    apkrt_syscall_dispatch(&frame);
    return static_cast<long>(frame.x[0]);
}

/// A WebView request as the host's handler saw it: the payload copied out (its pointer is valid only during the call),
/// and an ANSWER_FD's bytes read from the owned descriptor, which the handler then closes.
struct Seen {
    apkrt_webview_call call;
    std::vector<std::uint8_t> payload;
    std::string fd_data;
    bool signal_blocked = false;
};

std::mutex seen_lock;
std::vector<Seen> seen;

/// Whether the calling thread blocks SIGURG, the runtime signal, as the host's handlers must run.
bool runtime_signal_blocked() {
    sigset_t blocked;
    ::pthread_sigmask(SIG_BLOCK, nullptr, &blocked);
    return sigismember(&blocked, SIGURG) == 1;
}

void on_webview(void *context, const apkrt_webview_call *c) {
    CHECK(context == &seen);
    Seen record;
    record.call = *c;
    if (c->payload != nullptr) {
        const auto *bytes = static_cast<const std::uint8_t *>(c->payload);
        record.payload.assign(bytes, bytes + c->payload_length);
    }
    record.call.payload = nullptr;  // the runtime frees it when the handler returns
    if ((c->flags & APKRT_WEBVIEW_ANSWER_FD) != 0 && c->fd >= 0) {
        char buffer[256];
        const ssize_t n = ::read(c->fd, buffer, sizeof buffer);
        if (n > 0) record.fd_data.assign(buffer, static_cast<std::size_t>(n));
        ::close(c->fd);  // the host owns the dup the runtime handed it
    }
    record.signal_blocked = runtime_signal_blocked();
    const std::lock_guard<std::mutex> hold(seen_lock);
    seen.push_back(std::move(record));
}

/// The calls seen since the last take, in order.
std::vector<Seen> take() {
    const std::lock_guard<std::mutex> hold(seen_lock);
    return std::exchange(seen, {});
}

void on_end(void *, int) {}

/// The one session of the process, begun once with the webview handler.
void ensure_begun() {
    static const bool ok = [] {
        apkrt_session session{};
        session.context = &seen;
        session.end = on_end;
        session.webview = on_webview;
        return apkrt_session_begin(&session) == 0;
    }();
    CHECK(ok);
}

/// A request with op, token and fd (-1) set; the caller fills the rest.
apkrt_webview_request make_request(std::uint32_t op, std::uint64_t token) {
    apkrt_webview_request request{};
    request.op = op;
    request.token = token;
    request.fd = -1;
    return request;
}

long send(const apkrt_webview_request &request) {
    return call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_WEBVIEW, address(&request));
}

/// The real read end behind a guest channel fd, so the header-only reader can be driven in this host process.
int read_fd(long guest_fd) {
    const auto description = apkrt::descriptors::get(static_cast<int>(guest_fd));
    return description ? description->host() : -1;
}

}  // namespace

TEST(a_make_opens_a_channel_reports_to_the_host_and_the_channel_works) {
    ensure_begun();
    const long fd = send(make_request(APKRT_WEBVIEW_MAKE, 1));
    CHECK(fd >= 0);
    const auto calls = take();
    CHECK(calls.size() == 1);
    CHECK(calls[0].call.op == APKRT_WEBVIEW_MAKE && calls[0].call.token == 1 && calls[0].signal_blocked);
    // The channel is a real one: the host posts and the guest reads it back.
    CHECK(apkrt_webview_post(1, APKRT_WEBVIEW_REC_CREATED, 0, nullptr, 0) == 0);
    WebViewReader reader(read_fd(fd));
    WebViewRecord rec;
    CHECK(reader.next(rec) == 1 && rec.type == APKRT_WEBVIEW_REC_CREATED);
    CHECK(send(make_request(APKRT_WEBVIEW_MAKE, 1)) == -eexist);  // one live channel per token
    CHECK(send(make_request(APKRT_WEBVIEW_REMOVE, 1)) == 0);
    take();
}

TEST(a_bad_address_op_or_token_is_refused) {
    ensure_begun();
    CHECK(call(APKRT_NR_SESSION_REQUEST, APKRT_REQUEST_WEBVIEW, 0) == -efault);  // no struct
    CHECK(send(make_request(0, 1)) == -einval);                                  // op below MAKE
    CHECK(send(make_request(APKRT_WEBVIEW_ANSWER + 1, 1)) == -einval);           // op above ANSWER
    CHECK(send(make_request(APKRT_WEBVIEW_MAKE, 0)) == -einval);                 // token 0
    CHECK(take().empty());  // nothing reached the host
}

TEST(place_validates_the_frame_and_forwards_the_rects) {
    ensure_begun();
    CHECK(send(make_request(APKRT_WEBVIEW_MAKE, 2)) >= 0);
    take();
    auto place = make_request(APKRT_WEBVIEW_PLACE, 2);
    place.window = 70;
    place.left = 4;
    place.top = 8;
    place.right = 204;
    place.bottom = 308;
    place.clip_left = 4;
    place.clip_top = 8;
    place.clip_right = 204;
    place.clip_bottom = 150;  // clipped shorter than the frame
    CHECK(send(place) == 0);
    const auto calls = take();
    CHECK(calls.size() == 1);
    const apkrt_webview_call &c = calls[0].call;
    CHECK(c.op == APKRT_WEBVIEW_PLACE && c.token == 2 && c.window == 70);
    CHECK(c.left == 4 && c.top == 8 && c.right == 204 && c.bottom == 308);
    CHECK(c.clip_left == 4 && c.clip_top == 8 && c.clip_right == 204 && c.clip_bottom == 150);

    auto empty = make_request(APKRT_WEBVIEW_PLACE, 2);
    empty.window = 70;
    empty.right = 10;
    empty.bottom = 10;
    empty.left = 10;  // right == left: empty frame
    empty.top = 0;
    CHECK(send(empty) == -einval);
    auto no_window = make_request(APKRT_WEBVIEW_PLACE, 2);
    no_window.right = 10;
    no_window.bottom = 10;  // window 0
    CHECK(send(no_window) == -einval);
    auto unknown = make_request(APKRT_WEBVIEW_PLACE, 999);
    unknown.window = 70;
    unknown.right = 10;
    unknown.bottom = 10;
    CHECK(send(unknown) == -enoent);
    CHECK(send(make_request(APKRT_WEBVIEW_REMOVE, 2)) == 0);
    take();
}

TEST(call_copies_the_payload_and_forwards_it) {
    ensure_begun();
    CHECK(send(make_request(APKRT_WEBVIEW_MAKE, 3)) >= 0);
    take();
    const std::string script = "alert('minato')";
    auto js = make_request(APKRT_WEBVIEW_CALL, 3);
    js.request_id = 55;
    js.payload = address(script.data());
    js.payload_length = static_cast<std::uint32_t>(script.size());
    CHECK(send(js) == 0);
    const auto calls = take();
    CHECK(calls.size() == 1);
    CHECK(calls[0].call.op == APKRT_WEBVIEW_CALL && calls[0].call.request_id == 55);
    CHECK(calls[0].payload == std::vector<std::uint8_t>(script.begin(), script.end()));

    auto big = make_request(APKRT_WEBVIEW_CALL, 3);
    big.payload_length = APKRT_WEBVIEW_RECORD_MAX + 1;  // over the inline cap
    big.payload = address(script.data());
    CHECK(send(big) == -einval);
    auto bad_flag = make_request(APKRT_WEBVIEW_CALL, 3);
    bad_flag.flags = APKRT_WEBVIEW_ANSWER_FD;  // the fd mode is for ANSWER only
    CHECK(send(bad_flag) == -einval);
    auto no_payload = make_request(APKRT_WEBVIEW_CALL, 3);
    no_payload.payload_length = 8;  // a length with a null address
    CHECK(send(no_payload) == -efault);
    CHECK(send(make_request(APKRT_WEBVIEW_CALL, 999)) == -enoent);
    CHECK(send(make_request(APKRT_WEBVIEW_REMOVE, 3)) == 0);
    take();
}

TEST(answer_carries_inline_bytes_or_an_owned_fd) {
    ensure_begun();
    CHECK(send(make_request(APKRT_WEBVIEW_MAKE, 4)) >= 0);
    take();
    // Inline: a prompt's reply.
    const std::string reply = "yes";
    auto inline_answer = make_request(APKRT_WEBVIEW_ANSWER, 4);
    inline_answer.request_id = 12;
    inline_answer.payload = address(reply.data());
    inline_answer.payload_length = static_cast<std::uint32_t>(reply.size());
    CHECK(send(inline_answer) == 0);
    auto calls = take();
    CHECK(calls.size() == 1 && calls[0].call.request_id == 12);
    CHECK(calls[0].payload == std::vector<std::uint8_t>(reply.begin(), reply.end()));
    CHECK(calls[0].call.fd == -1);

    // By fd: a resource body the host streams. Register a pipe's read end as the guest fd; the runtime hands the host
    // a dup, a different descriptor, whose bytes the host reads.
    int pipe_ends[2];
    CHECK(::pipe(pipe_ends) == 0);
    const std::string body = "vendor.js";
    CHECK(::write(pipe_ends[1], body.data(), body.size()) == static_cast<ssize_t>(body.size()));
    ::close(pipe_ends[1]);  // so the host's read sees the bytes then EOF
    const long guest_fd = apkrt::descriptors::add(pipe_ends[0], true);
    CHECK(guest_fd >= 0);
    auto fd_answer = make_request(APKRT_WEBVIEW_ANSWER, 4);
    fd_answer.request_id = 13;
    fd_answer.flags = APKRT_WEBVIEW_ANSWER_FD;
    fd_answer.fd = static_cast<std::int32_t>(guest_fd);
    fd_answer.offset = 0;
    fd_answer.length = -1;
    CHECK(send(fd_answer) == 0);
    calls = take();
    CHECK(calls.size() == 1 && calls[0].call.request_id == 13);
    CHECK((calls[0].call.flags & APKRT_WEBVIEW_ANSWER_FD) != 0);
    CHECK(calls[0].fd_data == body);  // the host read the dup'd fd's bytes

    auto bad_fd = make_request(APKRT_WEBVIEW_ANSWER, 4);
    bad_fd.flags = APKRT_WEBVIEW_ANSWER_FD;
    bad_fd.fd = 999999;  // not a descriptor
    CHECK(send(bad_fd) == -ebadf);
    CHECK(send(make_request(APKRT_WEBVIEW_ANSWER, 999)) == -enoent);
    CHECK(send(make_request(APKRT_WEBVIEW_REMOVE, 4)) == 0);
    take();
}

TEST(remove_closes_the_channel_and_is_idempotent_only_once) {
    ensure_begun();
    const long fd = send(make_request(APKRT_WEBVIEW_MAKE, 5));
    CHECK(fd >= 0);
    take();
    WebViewReader reader(read_fd(fd));
    CHECK(send(make_request(APKRT_WEBVIEW_REMOVE, 5)) == 0);
    const auto calls = take();
    CHECK(calls.size() == 1 && calls[0].call.op == APKRT_WEBVIEW_REMOVE && calls[0].call.token == 5);
    WebViewRecord rec;
    CHECK(reader.next(rec) == 0);  // the channel closed: EOF
    CHECK(send(make_request(APKRT_WEBVIEW_REMOVE, 5)) == -enoent);  // the token is gone
    CHECK(take().empty());
}

TEST(at_most_sixteen_views_live_at_once) {
    ensure_begun();
    const std::uint64_t base = 100;
    for (std::uint64_t i = 0; i < 16; ++i) CHECK(send(make_request(APKRT_WEBVIEW_MAKE, base + i)) >= 0);
    CHECK(send(make_request(APKRT_WEBVIEW_MAKE, base + 16)) == -enospc);  // the 17th
    take();
    for (std::uint64_t i = 0; i < 16; ++i) CHECK(send(make_request(APKRT_WEBVIEW_REMOVE, base + i)) == 0);
    // room again
    CHECK(send(make_request(APKRT_WEBVIEW_MAKE, base + 16)) >= 0);
    CHECK(send(make_request(APKRT_WEBVIEW_REMOVE, base + 16)) == 0);
    take();
}

int main() { return apk::testing::run_all(); }
