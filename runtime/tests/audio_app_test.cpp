// ADR-0036's aaudio-test and opensl-test, and ADR-0064's audio-input-test and its twin audio-input-test-granted (the
// microphone grant is stored before the start, and the input host captures silence), on the Mac: a scripted host
// (app_host.h) that runs the app of fixtures/apps through activity-host with the start of ADR-0028's test 3, and plays
// the host's audio side as the App's AudioHost does, without sound: a fake host of 48 kHz with IO buffers of 5 ms and
// 10 ms of output latency, whose consumer thread reads every open stream every 5 ms with apkrt_audio_read, as a render
// block would, the frames of 5 ms at the stream's rate. It passes when the app finishes by itself, its lines are
// fixtures/expected/<app>.logcat (tools/android-reference.sh --app <app>), the only request was FINISH, activity-host
// ended with status 0, the consumer read, and heard only the zeros both apps play, and every stream the host opened was
// closed once. The device lines and the consumer's counts are shown, not compared. audio_app_test <scratch directory>
// <fixtures bin> <bionic lib> <guest /system> <fixtures/expected> <app>
#include "app_host.h"

#include <mach/mach_time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace app_host;
using namespace std::chrono_literals;

constexpr double hardware_rate = 48'000, io_buffer = 0.005;
constexpr std::int64_t output_latency = 10'000'000;  // ns
constexpr auto period = 5ms;

/// A stream the fake host opened.
struct Stream {
    double rate;
    std::int32_t channels;
    double owed = 0;  // the frames of the periods so far, less those asked for
    std::int64_t reads = 0, frames = 0;
};

struct Host {
    std::mutex lock;
    std::map<std::uint64_t, Stream> open;
    std::vector<std::uint64_t> opened, closed;
    std::int64_t reads = 0, frames = 0, xruns = 0;
    std::int64_t loud = 0;  // samples read that are not the apps' zeros
};
Host host;

int open_stream(void *, std::uint64_t stream, double sample_rate, std::int32_t channels, std::int32_t,
                apkrt_audio_hardware *hardware) {
    const std::lock_guard hold(host.lock);
    host.open[stream] = {sample_rate > 0 ? sample_rate : hardware_rate, channels};
    host.opened.push_back(stream);
    *hardware = {hardware_rate, io_buffer};
    apkrt_audio_set_output(nullptr, output_latency);  // as the App's engine starts, from within the open
    return 0;
}

void close_stream(void *, std::uint64_t stream) {
    const std::lock_guard hold(host.lock);
    if (const auto found = host.open.find(stream); found != host.open.end()) {
        host.reads += found->second.reads;
        host.frames += found->second.frames;
        host.xruns += apkrt_audio_xruns(stream);
        host.open.erase(found);
    }
    host.closed.push_back(stream);
}

/// The render blocks of the open streams, every period.
void consume(const std::atomic<bool> &done) {
    std::vector<float> buffer;
    auto next = std::chrono::steady_clock::now();
    while (!done.load()) {
        next += period;
        std::this_thread::sleep_until(next);
        const std::lock_guard hold(host.lock);
        for (auto &[id, stream] : host.open) {
            stream.owed += stream.rate * std::chrono::duration<double>(period).count();
            const auto frames = static_cast<std::int64_t>(stream.owed);
            stream.owed -= static_cast<double>(frames);
            buffer.assign(static_cast<std::size_t>(frames * stream.channels), 0.0f);
            const std::int64_t read = apkrt_audio_read(id, buffer.data(), frames, mach_absolute_time());
            host.loud += std::count_if(buffer.begin(), buffer.begin() + read * stream.channels,
                                       [](float sample) { return sample != 0.0f; });
            stream.frames += read;
            ++stream.reads;
        }
    }
}

/// The input streams the fake input host opened: it captures silence, every period, into the runtime's ring.
std::map<std::uint64_t, Stream> inputs;

int open_input(void *, std::uint64_t stream, double sample_rate, std::int32_t channels, std::int32_t,
               apkrt_audio_hardware *hardware) {
    const std::lock_guard hold(host.lock);
    inputs[stream] = {sample_rate > 0 ? sample_rate : hardware_rate, channels};
    host.opened.push_back(stream);
    *hardware = {hardware_rate, io_buffer};
    return 0;
}

void close_input(void *, std::uint64_t stream) {
    const std::lock_guard hold(host.lock);
    inputs.erase(stream);
    host.closed.push_back(stream);
}

/// The microphone of the open input streams, every period: the frames of 5 ms at the stream's rate, zeros.
void capture_silence(const std::atomic<bool> &done) {
    std::vector<float> buffer;
    auto next = std::chrono::steady_clock::now();
    while (!done.load()) {
        next += period;
        std::this_thread::sleep_until(next);
        const std::lock_guard hold(host.lock);
        for (auto &[id, stream] : inputs) {
            stream.owed += stream.rate * std::chrono::duration<double>(period).count();
            const auto frames = static_cast<std::int64_t>(stream.owed);
            stream.owed -= static_cast<double>(frames);
            buffer.assign(static_cast<std::size_t>(frames * stream.channels), 0.0f);
            stream.frames += apkrt_audio_input_write(id, buffer.data(), frames, mach_absolute_time());
        }
    }
}

bool requested(const std::string &request) {
    return std::find(seen.requests.begin(), seen.requests.end(), request) != seen.requests.end();
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 7) {
        std::fprintf(stderr, "usage: audio_app_test <scratch> <fixtures bin> <bionic lib> <guest system> <expected> "
                             "<aaudio-test | opensl-test | sdl-audio-test>\n");
        return 2;
    }
    const Paths paths{argv[1], argv[2], argv[3], argv[4], argv[5]};
    const std::string name = argv[6];
    const bool microphone = name == "audio-input-test" || name == "audio-input-test-granted";
    if (name != "aaudio-test" && name != "opensl-test" && name != "sdl-audio-test" && !microphone) {
        fail("no audio app " + name);
        return 1;
    }
    App app{.name = name, .package = name == "aaudio-test"       ? "io.github.kisame76.minato.test.aaudio"
                                     : name == "audio-input-test" ? "io.github.kisame76.minato.test.audioinput"
                                     : microphone                 ? "io.github.kisame76.minato.test.audioinputgranted"
                                                                  : "io.github.kisame76.minato.test.opensl"};
    const char *library = name == "aaudio-test" || microphone ? "libaaudio.so" : "libOpenSLES.so";
    if (!std::filesystem::exists(paths.bionic / library)) {
        std::printf("skipped: %s is not built (tools/build-bionic.sh)\n", (paths.bionic / library).c_str());
        return 0;
    }
    if (!prepare(app, paths, name)) return 0;

    Capture capture;
    if (!capture.start()) {
        fail("cannot capture stderr");
        return 1;
    }
    const apkrt_audio_host audio{nullptr, open_stream, close_stream};
    if (apkrt_set_audio_host(&audio) != 0) fail("apkrt_set_audio_host refused the host");
    const apkrt_audio_input_host microphone_host{nullptr, open_input, close_input};
    if (apkrt_set_audio_input_host(&microphone_host) != 0) fail("apkrt_set_audio_input_host refused the host");
    if (microphone) {  // the host records the grant at a session's begin
        apkrt_permission_answer(0, APKRT_PERMISSION_MICROPHONE,
                                name == "audio-input-test-granted" ? APKRT_PERMISSION_GRANTED
                                                                    : APKRT_PERMISSION_DENIED);
    }
    std::atomic<bool> done{false};
    std::thread consumer(consume, std::cref(done));
    std::thread microphone_thread(capture_silence, std::cref(done));
    begin({3.0, width, height, 1, 1, 120, "de", "DE"});
    if (!start(app, app.guest + "/base.apk", name.c_str(), "ANativeActivity_onCreate")) _exit(1);

    // The start, then active: the window's gate opens. The app's test thread runs from android_main on and
    // finishes the app at its end.
    const std::uint64_t window = apkrt_window_add(nullptr, width, height);
    post(APKRT_EVENT_CREATE);
    post(APKRT_EVENT_START);
    post(APKRT_EVENT_RESUME);
    answered(show_window(window, true), "the start");
    apkrt_window_set_gate(window, 1);
    bool finished = false;  // not when the program ends first, as when it cannot start
    wait_for([&] { return (finished = requested("FINISH")) || seen.ended; }, 60s);
    if (!finished) fail("no FINISH request");
    quit(window);
    if (!wait_for([] { return seen.ended; }, 10s)) fail("the program did not end after onDestroy");
    done.store(true);
    consumer.join();
    microphone_thread.join();

    const std::string log = capture.drain();
    {
        std::lock_guard hold(seen.lock);
        if (seen.status != 0) fail("activity-host ended with status " + std::to_string(seen.status));
        std::string requests;
        for (const auto &request : seen.requests) requests += "[" + request + "]";
        if (requests != "[FINISH]") fail("the requests were " + requests);
    }
    const std::string app_lines = lines_with(log, "I/" + name + ": ");
    if (app_lines != read_text(paths.expected / (name + ".logcat"))) {
        fail("the app's lines differ from Android 15's:\n" + app_lines);
    }
    ::dprintf(report, "The device lines:\n%s", lines_with(log, "I/" + name + "-device: ").c_str());
    {
        const std::lock_guard hold(host.lock);
        if (!host.open.empty()) fail(std::to_string(host.open.size()) + " streams were not closed");
        auto closed = host.closed;
        std::sort(closed.begin(), closed.end());
        auto opened = host.opened;
        std::sort(opened.begin(), opened.end());
        if (closed != opened) fail("the streams closed are not those opened, once each");
        if (host.frames == 0 && !microphone) fail("the consumer read nothing");
        if (host.loud != 0) fail("the app plays zeros, but the consumer heard " + std::to_string(host.loud) + " other samples");
        ::dprintf(report, "The consumer: %zu streams, %lld reads, %lld frames, %lld xruns\n", host.opened.size(),
                  static_cast<long long>(host.reads), static_cast<long long>(host.frames),
                  static_cast<long long>(host.xruns));
    }
    if (failures == 0) {
        ::dprintf(report, "PASS: %s\n", name.c_str());
    } else {
        ::dprintf(report, "%d failures\n", failures);
    }
    _exit(failures == 0 ? 0 : 1);
}
