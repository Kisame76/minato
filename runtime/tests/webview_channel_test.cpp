// Checks the WebView channel (ADR-0056 step a): webview_make opens a host -> guest record stream; webview_post frames
// records (one per payload up to APKRT_WEBVIEW_RECORD_MAX, a larger payload split with APKRT_WEBVIEW_MORE) and a
// WebViewReader reads whole records back, joining partial reads and MORE chains and keeping each record's type and
// request_id; a payload over the 1 MiB cap goes through chunking; a header with an unknown type or an over-cap length
// is refused with EINVAL; and the reader sees EOF when the channel closes. The test's thread plays the guest and posts
// as the host, as channels_test.cpp does. Each test uses its own token so the global channel table does not cross them.
#include "channels.h"
#include "descriptors.h"

#include "APKRuntime/apkrt_webview_reader.h"
#include "APKRuntime/guest_interface.h"
#include "check.h"

#include <fcntl.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace {

using apkrt::channels::WebViewReader;
using apkrt::channels::WebViewRecord;
using Bytes = std::vector<std::uint8_t>;

constexpr std::int64_t einval = 22;

/// The real read end behind a guest channel fd, so the header-only reader (which read()s the fd directly, as the guest
/// does) can be driven in this host process. The guest's read() of the channel fd waits in the runtime on this end,
/// which is non-blocking on the host (blocking_test.cpp checks that read); the test's direct reads make it blocking, so
/// that they wait as the guest's do.
int read_fd(long guest_fd) {
    const auto description = apkrt::descriptors::get(static_cast<int>(guest_fd));
    if (!description) return -1;
    ::fcntl(description->host(), F_SETFL, ::fcntl(description->host(), F_GETFL) & ~O_NONBLOCK);
    return description->host();
}

/// A payload of `n` bytes with a recognizable pattern, so a wrong join or a truncation shows.
Bytes pattern(std::size_t n) {
    Bytes out(n);
    for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<std::uint8_t>((i * 7 + 3) & 0xff);
    return out;
}

long post(std::uint64_t token, std::uint32_t type, std::uint64_t request_id, const Bytes &payload) {
    return apkrt::channels::webview_post(token, type, request_id, payload.data(), payload.size());
}

}  // namespace

TEST(webview_make_opens_a_channel_with_a_fd) {
    const long fd = apkrt::channels::webview_make(1);
    CHECK(fd >= 0);
    CHECK(apkrt::channels::webview_close(1) == 0);
}

TEST(a_posted_record_keeps_its_type_request_id_and_payload) {
    const std::uint64_t token = 2;
    const long fd = apkrt::channels::webview_make(token);
    CHECK(fd >= 0);
    WebViewReader reader(read_fd(fd));
    const Bytes body = pattern(37);
    CHECK(post(token, APKRT_WEBVIEW_REC_NAVIGATION, 0, body) == 0);
    CHECK(post(token, APKRT_WEBVIEW_REC_JS_RESULT, 99, pattern(10)) == 0);
    WebViewRecord first;
    CHECK(reader.next(first) == 1);
    CHECK(first.type == APKRT_WEBVIEW_REC_NAVIGATION && first.request_id == 0 && first.payload == body);
    WebViewRecord second;
    CHECK(reader.next(second) == 1);
    CHECK(second.type == APKRT_WEBVIEW_REC_JS_RESULT && second.request_id == 99 && second.payload == pattern(10));
    apkrt::channels::webview_close(token);
}

TEST(a_one_mebibyte_record_at_the_cap_round_trips) {
    const std::uint64_t token = 3;
    const long fd = apkrt::channels::webview_make(token);
    CHECK(fd >= 0);
    WebViewReader reader(read_fd(fd));
    const Bytes body = pattern(APKRT_WEBVIEW_RECORD_MAX);  // exactly the cap: one record
    CHECK(post(token, APKRT_WEBVIEW_REC_RESOURCE, 0, body) == 0);
    WebViewRecord record;
    CHECK(reader.next(record) == 1);
    CHECK(record.type == APKRT_WEBVIEW_REC_RESOURCE && record.payload == body);
    apkrt::channels::webview_close(token);
}

TEST(a_payload_larger_than_the_cap_is_chunked_and_reassembled) {
    const std::uint64_t token = 4;
    const long fd = apkrt::channels::webview_make(token);
    CHECK(fd >= 0);
    WebViewReader reader(read_fd(fd));
    const Bytes body = pattern(APKRT_WEBVIEW_RECORD_MAX * 2 + 123);  // three records: two full, one partial
    CHECK(post(token, APKRT_WEBVIEW_REC_JS_RESULT, 7, body) == 0);
    WebViewRecord record;
    CHECK(reader.next(record) == 1);  // the reader joins the MORE chain into one logical record
    CHECK(record.type == APKRT_WEBVIEW_REC_JS_RESULT && record.request_id == 7 && record.payload == body);
    apkrt::channels::webview_close(token);
}

TEST(a_record_delivered_in_pieces_is_read_whole) {
    const std::uint64_t token = 5;
    const long fd = apkrt::channels::webview_make(token);
    CHECK(fd >= 0);
    WebViewReader reader(read_fd(fd));
    // Frame one record by hand and deliver it in two raw writes with a gap, so the reader must wait for the rest.
    const Bytes body = pattern(5000);
    apkrt_webview_record_header header{};
    header.type = APKRT_WEBVIEW_REC_RESOURCE;
    header.length = static_cast<std::uint32_t>(body.size());
    header.request_id = 42;
    Bytes framed(sizeof header);
    std::memcpy(framed.data(), &header, sizeof header);
    framed.insert(framed.end(), body.begin(), body.end());
    const std::size_t cut = sizeof header + 1000;  // a cut inside the payload
    std::thread poster([&] {
        apkrt::channels::webview_post_raw(token, framed.data(), cut);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        apkrt::channels::webview_post_raw(token, framed.data() + cut, framed.size() - cut);
    });
    WebViewRecord record;
    const long got = reader.next(record);  // blocks until the whole record has arrived
    poster.join();
    CHECK(got == 1);
    CHECK(record.type == APKRT_WEBVIEW_REC_RESOURCE && record.request_id == 42 && record.payload == body);
    apkrt::channels::webview_close(token);
}

TEST(four_records_keep_their_request_ids) {
    const std::uint64_t token = 6;
    const long fd = apkrt::channels::webview_make(token);
    CHECK(fd >= 0);
    WebViewReader reader(read_fd(fd));
    const std::uint64_t ids[] = {30, 10, 40, 20};  // out of order
    for (const std::uint64_t id : ids) CHECK(post(token, APKRT_WEBVIEW_REC_JS_RESULT, id, pattern(id)) == 0);
    for (const std::uint64_t id : ids) {
        WebViewRecord record;
        CHECK(reader.next(record) == 1);
        CHECK(record.request_id == id && record.type == APKRT_WEBVIEW_REC_JS_RESULT && record.payload == pattern(id));
    }
    apkrt::channels::webview_close(token);
}

TEST(a_header_with_a_bad_length_or_an_unknown_type_is_refused) {
    {
        const std::uint64_t token = 7;
        const long fd = apkrt::channels::webview_make(token);
        CHECK(fd >= 0);
        WebViewReader reader(read_fd(fd));
        apkrt_webview_record_header bad{};
        bad.type = APKRT_WEBVIEW_REC_RESOURCE;
        bad.length = APKRT_WEBVIEW_RECORD_MAX + 1;  // over the cap
        CHECK(apkrt::channels::webview_post_raw(token, &bad, sizeof bad) == 0);
        WebViewRecord record;
        CHECK(reader.next(record) == -einval);
        apkrt::channels::webview_close(token);
    }
    {
        const std::uint64_t token = 8;
        const long fd = apkrt::channels::webview_make(token);
        CHECK(fd >= 0);
        WebViewReader reader(read_fd(fd));
        apkrt_webview_record_header bad{};
        bad.type = 9999;  // not a known APKRT_WEBVIEW_REC_*
        bad.length = 0;
        CHECK(apkrt::channels::webview_post_raw(token, &bad, sizeof bad) == 0);
        WebViewRecord record;
        CHECK(reader.next(record) == -einval);
        apkrt::channels::webview_close(token);
    }
}

TEST(the_reader_sees_eof_when_the_channel_closes) {
    const std::uint64_t token = 9;
    const long fd = apkrt::channels::webview_make(token);
    CHECK(fd >= 0);
    WebViewReader reader(read_fd(fd));
    CHECK(post(token, APKRT_WEBVIEW_REC_CREATED, 0, Bytes{}) == 0);
    CHECK(apkrt::channels::webview_close(token) == 0);
    WebViewRecord record;
    CHECK(reader.next(record) == 1);  // the record queued before the close still arrives
    CHECK(record.type == APKRT_WEBVIEW_REC_CREATED);
    WebViewRecord after;
    CHECK(reader.next(after) == 0);  // then EOF
}

TEST(the_webchrome_record_types_round_trip) {
    // The types added for WebChromeClient after App's step-b review (JS_DIALOG, CONSOLE, PROGRESS, TITLE) are known to
    // the reader now, so they carry a payload whole like the others.
    const std::uint64_t token = 10;
    const long fd = apkrt::channels::webview_make(token);
    CHECK(fd >= 0);
    WebViewReader reader(read_fd(fd));
    const std::uint32_t types[] = {APKRT_WEBVIEW_REC_JS_DIALOG, APKRT_WEBVIEW_REC_CONSOLE, APKRT_WEBVIEW_REC_PROGRESS,
                                   APKRT_WEBVIEW_REC_TITLE};
    for (const std::uint32_t type : types) CHECK(post(token, type, type, pattern(type)) == 0);
    for (const std::uint32_t type : types) {
        WebViewRecord record;
        CHECK(reader.next(record) == 1);
        CHECK(record.type == type && record.request_id == type && record.payload == pattern(type));
    }
    apkrt::channels::webview_close(token);
}

TEST(a_cookies_record_round_trips) {
    // ADR-0056 c3's COOKIES (14), the data store's whole list: known to the reader, joined across MORE like the others.
    const std::uint64_t token = 11;
    const long fd = apkrt::channels::webview_make(token);
    CHECK(fd >= 0);
    WebViewReader reader(read_fd(fd));
    const Bytes body = pattern(APKRT_WEBVIEW_RECORD_MAX + 100);
    CHECK(post(token, APKRT_WEBVIEW_REC_COOKIES, 0, body) == 0);
    WebViewRecord record;
    CHECK(reader.next(record) == 1);
    CHECK(record.type == APKRT_WEBVIEW_REC_COOKIES && record.request_id == 0 && record.payload == body);
    apkrt::channels::webview_close(token);
}

int main() { return apk::testing::run_all(); }
