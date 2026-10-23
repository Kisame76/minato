// The sensor transport (ADR-0061): APKRT_NR_SENSOR through the real dispatcher, with a fake host. It checks the guest
// side the hub's own test does not reach — the syscall validation (bad op, bad pointer, bad descriptor), the queue as a
// guest descriptor (opened, read through the guest read syscall, closed by its destructor), and the sensor list.

#include "sensor_hub.h"
#include "syscall_frame.h"

#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <vector>

#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"
#include "descriptors.h"
#include "check.h"

namespace {

constexpr long efault = 14, ebadf = 9, einval = 22;
constexpr std::uint64_t nr_read = 63, nr_write = 64;  // Linux arm64 read, write
constexpr std::int32_t ACCEL = 1, GYRO = 4, HIGH = 3;

template <typename T>
std::uint64_t address(T *pointer) {
    return reinterpret_cast<std::uint64_t>(pointer);
}

/// A syscall of the calling host thread, straight into the dispatcher.
long call(std::uint64_t number, std::uint64_t a0 = 0, std::uint64_t a1 = 0, std::uint64_t a2 = 0) {
    apkrt_syscall_frame frame{};
    frame.x[0] = a0;
    frame.x[1] = a1;
    frame.x[2] = a2;
    frame.x[8] = number;
    apkrt_syscall_dispatch(&frame);
    return static_cast<long>(frame.x[0]);
}

long sensor(std::uint64_t op, std::uint64_t argument = 0) { return call(APKRT_NR_SENSOR, op, argument); }

// A fake CoreMotion host: records which types have updates on.
struct FakeHost {
    std::mutex mutex;
    std::set<std::int32_t> on;
};
FakeHost host;

int fake_start(void *context, std::int32_t type, std::int64_t) {
    auto *self = static_cast<FakeHost *>(context);
    const std::lock_guard<std::mutex> hold(self->mutex);
    self->on.insert(type);
    return 0;
}
void fake_stop(void *context, std::int32_t type) {
    auto *self = static_cast<FakeHost *>(context);
    const std::lock_guard<std::mutex> hold(self->mutex);
    self->on.erase(type);
}
bool started(std::int32_t type) {
    const std::lock_guard<std::mutex> hold(host.mutex);
    return host.on.count(type) != 0;
}

void install(std::initializer_list<std::int32_t> available) {
    apkrt::descriptors::reset();
    apkrt::sensors::reset();
    {
        const std::lock_guard<std::mutex> hold(host.mutex);
        host.on.clear();
    }
    const std::vector<std::int32_t> list(available);
    const apkrt_sensor_host interface{&host, fake_start, fake_stop};
    apkrt_sensor_set_host(&interface, list.data(), list.size());
}

long open_queue_fd() { return sensor(APKRT_SENSOR_OPEN); }

long enable(long fd, std::int32_t type) {
    apkrt_sensor_request request{static_cast<std::int32_t>(fd), type, 20'000'000};
    return sensor(APKRT_SENSOR_ENABLE, address(&request));
}

}  // namespace

TEST(the_sensor_list_syscall_fills_the_buffer) {
    install({ACCEL, GYRO});
    apkrt_sensor_list buffer{};
    const long count = sensor(APKRT_SENSOR_LIST, address(&buffer));
    CHECK(count == 2);
    CHECK(buffer.count == 2);
    if (buffer.count == 2) {
        CHECK(buffer.sensors[0].type == ACCEL && buffer.sensors[0].value_count == 3 && buffer.sensors[0].name[0] != '\0');
        CHECK(buffer.sensors[1].type == GYRO && buffer.sensors[1].value_count == 3);
    }
    // A list into gone memory is -EFAULT, not a crash.
    const auto page = static_cast<std::size_t>(::getpagesize());
    void *const gone = ::mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(gone != MAP_FAILED);
    CHECK(::munmap(gone, page) == 0);
    CHECK(sensor(APKRT_SENSOR_LIST, reinterpret_cast<std::uint64_t>(gone)) == -efault);
}

TEST(a_queue_opens_enables_and_refuses_a_bad_descriptor) {
    install({ACCEL});
    const long fd = open_queue_fd();
    CHECK(fd >= 0);
    if (fd < 0) return;
    CHECK(enable(fd, ACCEL) == 0);
    CHECK(started(ACCEL));
    CHECK(enable(99999, ACCEL) == -ebadf);        // not an open descriptor
    CHECK(enable(fd, 99) == -einval);             // not an available sensor type
    apkrt_sensor_request request{static_cast<std::int32_t>(fd), ACCEL, 0};
    CHECK(sensor(APKRT_SENSOR_DISABLE, address(&request)) == 0);
    CHECK(!started(ACCEL));
    apkrt::descriptors::close(static_cast<int>(fd));
}

TEST(a_push_reaches_the_descriptor_through_the_guest_read_syscall) {
    install({ACCEL});
    const long fd = open_queue_fd();
    CHECK(fd >= 0);
    if (fd < 0) return;
    CHECK(enable(fd, ACCEL) == 0);
    const float values[] = {1.5f, -2.5f, 9.8f};
    apkrt_sensor_push(ACCEL, values, 3, 100.0, HIGH);
    apkrt::sensors::Event event{};
    const long got = call(nr_read, static_cast<std::uint64_t>(fd), address(&event), sizeof event);
    CHECK(got == static_cast<long>(sizeof event));
    if (got == static_cast<long>(sizeof event)) {
        CHECK(event.sensor == ACCEL && event.type == ACCEL);
        CHECK(event.vector.x == 1.5f && event.vector.y == -2.5f && event.vector.z == 9.8f);
        CHECK(event.vector.status == HIGH);
    }
    apkrt::descriptors::close(static_cast<int>(fd));
}

TEST(a_bad_request_pointer_is_efault_and_an_unknown_op_is_einval) {
    install({ACCEL});
    const auto page = static_cast<std::size_t>(::getpagesize());
    void *const gone = ::mmap(nullptr, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(gone != MAP_FAILED);
    CHECK(::munmap(gone, page) == 0);
    CHECK(sensor(APKRT_SENSOR_ENABLE, reinterpret_cast<std::uint64_t>(gone)) == -efault);
    CHECK(sensor(999) == -einval);  // not an op
}

TEST(closing_the_descriptor_stops_the_sensor) {
    install({ACCEL});
    const long fd = open_queue_fd();
    CHECK(fd >= 0);
    if (fd < 0) return;
    CHECK(enable(fd, ACCEL) == 0);
    CHECK(started(ACCEL));
    apkrt::descriptors::close(static_cast<int>(fd));  // the descriptor's destructor tells the hub to stop its sensors
    CHECK(!started(ACCEL));
}

TEST(flush_via_the_syscall_writes_a_complete_event) {
    install({ACCEL});
    const long fd = open_queue_fd();
    CHECK(fd >= 0);
    if (fd < 0) return;
    CHECK(enable(fd, ACCEL) == 0);
    CHECK(sensor(APKRT_SENSOR_FLUSH, static_cast<std::uint64_t>(fd)) == 0);
    apkrt::sensors::Event event{};
    const long got = call(nr_read, static_cast<std::uint64_t>(fd), address(&event), sizeof event);
    CHECK(got == static_cast<long>(sizeof event));
    if (got == static_cast<long>(sizeof event)) {
        CHECK(event.type == apkrt::sensors::meta_type);
        CHECK(event.meta_data.what == apkrt::sensors::meta_flush_complete && event.meta_data.sensor == ACCEL);
    }
    apkrt::descriptors::close(static_cast<int>(fd));
}

// A config op on an open descriptor that is not a sensor queue (here a plain pipe) is refused — the dynamic_cast gate.
TEST(config_on_a_non_sensor_descriptor_is_ebadf) {
    install({ACCEL});
    int p[2];
    CHECK(::pipe(p) == 0);
    const long gfd = apkrt::descriptors::add(p[0], true);  // a plain Kind::file guest descriptor, not a SensorQueue
    ::close(p[1]);
    CHECK(gfd >= 0);
    if (gfd < 0) return;
    apkrt_sensor_request request{static_cast<std::int32_t>(gfd), ACCEL, 20'000'000};
    CHECK(sensor(APKRT_SENSOR_ENABLE, address(&request)) == -ebadf);
    CHECK(sensor(APKRT_SENSOR_FLUSH, static_cast<std::uint64_t>(gfd)) == -ebadf);
    apkrt::descriptors::close(static_cast<int>(gfd));
}

// The queue's guest fd is a read-only pipe end, so the guest cannot write into (forge) the channel.
TEST(a_guest_write_on_the_queue_fd_fails) {
    install({ACCEL});
    const long fd = open_queue_fd();
    CHECK(fd >= 0);
    if (fd < 0) return;
    const char byte = 0;
    CHECK(call(nr_write, static_cast<std::uint64_t>(fd), address(&byte), 1) < 0);
    apkrt::descriptors::close(static_cast<int>(fd));
}

// The queue lives on the description, not a single fd: closing one dup keeps its sensors on; the last close stops them.
TEST(closing_one_dup_keeps_the_sensor_on) {
    install({ACCEL});
    const long fd = open_queue_fd();
    CHECK(fd >= 0);
    if (fd < 0) return;
    CHECK(enable(fd, ACCEL) == 0);
    CHECK(started(ACCEL));
    const long dup = apkrt::descriptors::duplicate(static_cast<int>(fd), 0, false);
    CHECK(dup >= 0);
    apkrt::descriptors::close(static_cast<int>(fd));  // one of the two descriptions goes
    CHECK(started(ACCEL));                            // the queue lives on via the dup, so the sensor stays on
    apkrt::descriptors::close(static_cast<int>(dup));  // the last one
    CHECK(!started(ACCEL));
}

int main() { return apk::testing::run_all(); }
