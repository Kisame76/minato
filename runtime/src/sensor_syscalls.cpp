#include "sensor_syscalls.h"

#include <unistd.h>

#include <cstddef>
#include <cstring>
#include <memory>
#include <new>

#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"
#include "descriptors.h"
#include "sensor_hub.h"
#include "syscalls.h"

namespace apkrt::sensors {
namespace {

namespace linux_errno = sys::linux_errno;

// EMFILE (too many open descriptors) is not in sys::linux_errno; descriptors.cpp defines its own the same way.
constexpr long emfile = 24;

// A guest sensor queue. Its guest descriptor is the hub pipe's read end, held as a plain Kind::file description so the
// guest's read/poll/close use it directly. Closing it (ASensorManager_destroyEventQueue, or any close) runs this
// destructor, which tells the hub to drop the queue — the hub closes its write end and stops the sensors this queue was
// the last to want. The base destructor then closes the read end.
class SensorQueue final : public descriptors::Description {
public:
    SensorQueue(int read_end, int hub_id)
        : Description(read_end, /*can_block=*/true, /*adopted=*/false, /*nonblocking=*/true, Kind::file,
                      Description::PipeEnd::reading),  // the read end; a kqueue filter fires when the hub's write end goes
          hub_id_(hub_id) {}
    ~SensorQueue() override { close_queue(hub_id_); }
    SensorQueue(const SensorQueue &) = delete;
    SensorQueue &operator=(const SensorQueue &) = delete;

    int hub_id() const { return hub_id_; }

private:
    const int hub_id_;
};

/// The hub id of the sensor queue behind the guest descriptor `fd`, or -1 when `fd` is not one of ours.
int hub_id_of(int fd) {
    const auto queue = std::dynamic_pointer_cast<SensorQueue>(descriptors::get(fd));
    return queue ? queue->hub_id() : -1;
}

/// Reads `size` bytes from the guest at `address` into `out`; false (answer -EFAULT) on a bad pointer.
bool take(std::uint64_t address, void *out, std::size_t size) {
    return sys::memory::copy_checked(reinterpret_cast<std::uint64_t>(out), address, size);
}

/// Writes `size` bytes to the guest at `address` from `in`; false (answer -EFAULT) on a bad pointer.
bool give(std::uint64_t address, const void *in, std::size_t size) {
    return sys::memory::copy_checked(address, reinterpret_cast<std::uint64_t>(in), size);
}

long new_queue() {
    const auto opened = open_queue();
    if (!opened) return -emfile;  // no pipe (host descriptors exhausted) or no flag could be set
    std::shared_ptr<SensorQueue> queue;
    try {
        queue = std::make_shared<SensorQueue>(opened->read_fd, opened->id);
    } catch (const std::bad_alloc &) {
        close_queue(opened->id);  // drop the hub queue (closes its write end); the read end is still ours
        ::close(opened->read_fd);
        return -linux_errno::enomem;
    }
    // add adopts the read end and returns the guest number; on a full table it drops the queue, whose destructor closes
    // the hub side and the read end.
    return descriptors::add(std::move(queue), /*close_on_exec=*/true);
}

long configure(std::uint64_t argument, unsigned op) {
    apkrt_sensor_request request{};
    if (!take(argument, &request, sizeof request)) return -linux_errno::efault;
    const int hub_id = hub_id_of(request.queue);
    if (hub_id < 0) return -linux_errno::ebadf;
    switch (op) {
        case APKRT_SENSOR_ENABLE:
            return enable(hub_id, request.type, request.period_ns);
        case APKRT_SENSOR_DISABLE:
            return disable(hub_id, request.type);
        default:  // APKRT_SENSOR_RATE
            return set_rate(hub_id, request.type, request.period_ns);
    }
}

long flush_queue(int fd) {
    const int hub_id = hub_id_of(fd);
    if (hub_id < 0) return -linux_errno::ebadf;
    return flush(hub_id);
}

void fill_info(apkrt_sensor_info &out, const SensorInfo &info) {
    out = {};
    out.type = info.type;
    out.value_count = info.value_count;
    out.version = info.version;
    out.min_delay_us = info.min_delay_us;
    out.max_delay_us = info.max_delay_us;
    out.reporting_mode = info.reporting_mode;
    out.flags = info.flags;
    out.max_range = info.max_range;
    out.resolution = info.resolution;
    out.power_ma = info.power_ma;
    const auto copy = [](char (&field)[APKRT_SENSOR_NAME_MAX], const char *text) {
        std::strncpy(field, text != nullptr ? text : "", APKRT_SENSOR_NAME_MAX - 1);
        field[APKRT_SENSOR_NAME_MAX - 1] = '\0';
    };
    copy(out.name, info.name);
    copy(out.vendor, info.vendor);
    copy(out.string_type, info.string_type);
    copy(out.required_permission, info.required_permission);
}

long list(std::uint64_t argument) {
    SensorInfo table[APKRT_SENSOR_MAX];  // the hub offers at most APKRT_SENSOR_MAX (its fixed table's length)
    const std::size_t count = sensor_list(table, APKRT_SENSOR_MAX);
    apkrt_sensor_list out{};
    const std::size_t filled =
        count < static_cast<std::size_t>(APKRT_SENSOR_MAX) ? count : static_cast<std::size_t>(APKRT_SENSOR_MAX);
    out.count = static_cast<std::uint32_t>(filled);
    for (std::size_t i = 0; i < filled; ++i) fill_info(out.sensors[i], table[i]);
    if (!give(argument, &out, sizeof out)) return -linux_errno::efault;
    return static_cast<long>(count);
}

}  // namespace

long call(std::uint64_t op, std::uint64_t argument) try {
    switch (op) {
        case APKRT_SENSOR_LIST:
            return list(argument);
        case APKRT_SENSOR_OPEN:
            return argument == 0 ? new_queue() : -linux_errno::einval;  // OPEN takes no argument
        case APKRT_SENSOR_ENABLE:
        case APKRT_SENSOR_DISABLE:
        case APKRT_SENSOR_RATE:
            return configure(argument, static_cast<unsigned>(op));
        case APKRT_SENSOR_FLUSH:
            return flush_queue(static_cast<int>(argument));
        default:
            return -linux_errno::einval;
    }
} catch (const std::bad_alloc &) {
    return -linux_errno::enomem;  // a growing table ran out of memory: never let it unwind out of the dispatcher
}

}  // namespace apkrt::sensors
