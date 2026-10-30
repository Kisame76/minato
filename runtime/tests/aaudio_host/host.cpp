// The guest's calls and the fake audio host for the Mac builds of the guest's audio libraries (host.h).
#include "host.h"

#include "APKRuntime/guest_interface.h"
#include "syscall_frame.h"

#include <mach/mach_time.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

// MARK: - What the guest build gets from bionic

extern "C" int minato_host_syscall(int number, ...) {
    va_list arguments;
    va_start(arguments, number);
    apkrt_syscall_frame frame{};
    for (int index = 0; index < 4; ++index) frame.x[index] = va_arg(arguments, std::uint64_t);
    va_end(arguments);
    frame.x[8] = static_cast<std::uint64_t>(number);
    apkrt_syscall_dispatch(&frame);
    const auto result = static_cast<std::int64_t>(frame.x[0]);
    if (result >= -4095 && result < 0) {
        // bionic's syscall(): -1 and errno; Linux's numbers where Darwin's differ
        const auto linux_error = static_cast<int>(-result);
        errno = linux_error == 110 ? ETIMEDOUT : linux_error == 11 ? EAGAIN : linux_error;
        return -1;
    }
    return static_cast<int>(result);
}

namespace {

std::mutex log_lock;
std::map<std::string, int> lines_by_tag;

}  // namespace

extern "C" int __android_log_print(int, const char *tag, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    std::fprintf(stderr, "log %s: ", tag);
    const int written = std::vfprintf(stderr, format, arguments);
    std::fputc('\n', stderr);
    va_end(arguments);
    const std::lock_guard hold(log_lock);
    ++lines_by_tag[tag];
    return written;
}

namespace audio_host {

FakeHost fake;

namespace {

int fake_open(void *, std::uint64_t stream, double sample_rate, std::int32_t channels, std::int32_t performance_mode,
              apkrt_audio_hardware *hardware) {
    const std::lock_guard hold(fake.lock);
    fake.opened.push_back({stream, sample_rate, channels, performance_mode});
    *hardware = {48'000, fake.io_buffer};
    apkrt_audio_set_output(nullptr, 10'000'000);
    if (fake.disconnect_in_open) apkrt_audio_disconnected(stream);
    return 0;
}

void fake_close(void *, std::uint64_t stream) {
    const std::lock_guard hold(fake.lock);
    fake.closed.push_back(stream);
}

}  // namespace

void use_fake_host() {
    {
        const std::lock_guard hold(fake.lock);
        fake.opened.clear();
        fake.closed.clear();
        fake.disconnect_in_open = false;
        fake.io_buffer = 0.005;
    }
    static const apkrt_audio_host host{nullptr, fake_open, fake_close};
    if (apkrt_set_audio_host(&host) != 0) {  // every check after this would be about another host
        std::fprintf(stderr, "apkrt_set_audio_host refused the fake host\n");
        std::abort();
    }
}

Opened last_opened() {
    const std::lock_guard hold(fake.lock);
    return fake.opened.empty() ? Opened{} : fake.opened.back();
}

int log_lines(const char *tag) {
    const std::lock_guard hold(log_lock);
    const auto found = lines_by_tag.find(tag);
    return found == lines_by_tag.end() ? 0 : found->second;
}

std::vector<float> render(std::uint64_t handle, std::int64_t frames, std::int32_t channels) {
    std::vector<float> samples(static_cast<std::size_t>(frames * channels));
    const auto read = apkrt_audio_read(handle, samples.data(), frames, mach_absolute_time());
    samples.resize(static_cast<std::size_t>(read > 0 ? read * channels : 0));
    return samples;
}

bool stopped(std::uint64_t handle) {
    apkrt_audio_status status{};
    return minato_host_syscall(APKRT_NR_AUDIO_STATUS, handle, reinterpret_cast<std::uint64_t>(&status)) == 0 &&
           status.state == APKRT_AUDIO_STOPPED;
}

}  // namespace audio_host
