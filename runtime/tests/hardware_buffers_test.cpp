// Checks the guest's hardware buffers (ADR-0058 a)): hardware_buffer's operations through the syscall dispatcher, as
// libnativewindow's AHardwareBuffer_* make them, against a fake table of host buffers: the IDs, references and locks
// the runtime keeps, the checks that keep a host allocation sound, and what reaches the host. graphics/host's table on
// IOSurfaces has its own test, the guest's functions the fixture ahb-test against Android 15.
#include "APKRuntime/apkrt.h"
#include "APKRuntime/guest_interface.h"
#include "syscall_frame.h"
#include "syscalls.h"

#include "check.h"

#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <future>
#include <initializer_list>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace {

constexpr std::int64_t efault = 14, einval = 22, enomem = 12, enosys = 38;
constexpr std::uint32_t rgba8 = 1, rgbx8 = 2, rgb8 = 3, rgb565 = 4, rgba16f = 0x16, blob = 0x21, r8 = 0x38;
constexpr std::uint64_t cpu_read_often = 3, cpu_write_often = 3 << 4, gpu_sampled = 1 << 8;

static_assert(sizeof(apkrt_hardware_buffer) == 56);
static_assert(offsetof(apkrt_hardware_buffer, width) == 8 && offsetof(apkrt_hardware_buffer, format) == 20);
static_assert(offsetof(apkrt_hardware_buffer, usage) == 24 && offsetof(apkrt_hardware_buffer, stride) == 32);
static_assert(offsetof(apkrt_hardware_buffer, bytes_per_pixel) == 36);
static_assert(offsetof(apkrt_hardware_buffer, bytes_per_stride) == 40);
static_assert(offsetof(apkrt_hardware_buffer, reserved) == 44 && offsetof(apkrt_hardware_buffer, address) == 48);

std::int64_t call(std::uint64_t op, std::uint64_t argument) {
    apkrt_syscall_frame frame{};
    frame.x[0] = op;
    frame.x[1] = argument;
    frame.x[8] = APKRT_NR_HARDWARE_BUFFER;
    apkrt_syscall_dispatch(&frame);
    return static_cast<std::int64_t>(frame.x[0]);
}

std::int64_t call(std::uint64_t op, apkrt_hardware_buffer &buffer) {
    return call(op, reinterpret_cast<std::uint64_t>(&buffer));
}

/// A fake table of host buffers: memory of its own for each, rows rounded up to 16 pixels, and a log of the calls,
/// for any thread.
struct Fake {
    std::mutex mutex;
    struct Buffer {
        std::vector<unsigned char> bytes;
        std::uint32_t bytes_per_pixel = 0, stride = 0;
        int locks = 0;
    };
    std::map<std::uint64_t, Buffer> buffers;
    std::uint64_t next = 100;
    int attempts = 0, allocations = 0, frees = 0, lock_calls = 0, unlock_calls = 0;
    int refuse = 0;  // an allocate's answer when not 0
    int refuse_lock = 0, refuse_unlock = 0;  // a lock's or an unlock's answer when not 0, which then changes nothing
    int supported_answer = 0;  // supported's answer when not 0
    std::function<void()> while_locking, while_freeing;  // run at a lock's or a free's start, under no lock
    std::set<std::uint32_t> formats{rgba8, rgbx8, rgb565, rgba16f, blob, r8};

    static std::uint32_t bytes_per_pixel(std::uint32_t format) {
        switch (format) {
            case rgba16f: return 8;
            case rgb565: return 2;
            case blob:
            case r8: return 1;
            default: return 4;
        }
    }
};

Fake *fake_of(void *context) {
    return static_cast<Fake *>(context);
}

int fake_supported(void *context, const apkrt_hardware_buffer_description *description) {
    const std::lock_guard hold(fake_of(context)->mutex);
    if (fake_of(context)->supported_answer != 0) return fake_of(context)->supported_answer;
    return fake_of(context)->formats.count(description->format) != 0 ? 1 : 0;
}

int fake_allocate(void *context, const apkrt_hardware_buffer_description *description, std::uint64_t *handle,
                  std::uint32_t *stride) {
    Fake &fake = *fake_of(context);
    const std::lock_guard hold(fake.mutex);
    ++fake.attempts;
    if (fake.refuse != 0) return fake.refuse;
    if (fake.formats.count(description->format) == 0) return -static_cast<int>(einval);
    Fake::Buffer buffer;
    buffer.bytes_per_pixel = Fake::bytes_per_pixel(description->format);
    buffer.stride = description->format == blob ? description->width : (description->width + 15) & ~15u;
    buffer.bytes.resize(std::size_t{buffer.stride} * buffer.bytes_per_pixel * description->height *
                        description->layers);
    *handle = fake.next++;
    *stride = buffer.stride;
    fake.buffers[*handle] = std::move(buffer);
    ++fake.allocations;
    return 0;
}

int fake_lock(void *context, std::uint64_t handle, std::uint64_t, void **address, std::uint32_t *bytes_per_pixel,
              std::uint32_t *bytes_per_row) {
    Fake &fake = *fake_of(context);
    if (fake.while_locking) fake.while_locking();
    const std::lock_guard hold(fake.mutex);
    if (fake.refuse_lock != 0) return fake.refuse_lock;
    ++fake.lock_calls;
    auto &buffer = fake.buffers.at(handle);
    ++buffer.locks;
    *address = buffer.bytes.data();
    *bytes_per_pixel = buffer.bytes_per_pixel;
    *bytes_per_row = buffer.stride * buffer.bytes_per_pixel;
    return 0;
}

int fake_unlock(void *context, std::uint64_t handle) {
    Fake &fake = *fake_of(context);
    const std::lock_guard hold(fake.mutex);
    if (fake.refuse_unlock != 0) return fake.refuse_unlock;
    ++fake.unlock_calls;
    --fake.buffers.at(handle).locks;
    return 0;
}

void fake_free(void *context, std::uint64_t handle) {
    Fake &fake = *fake_of(context);
    if (fake.while_freeing) fake.while_freeing();
    const std::lock_guard hold(fake.mutex);
    CHECK(fake.buffers.at(handle).locks == 0);  // every lock ended first
    fake.buffers.erase(handle);
    ++fake.frees;
}

apkrt_hardware_buffers table_of(Fake &fake) {
    return {fake_supported, fake_allocate, fake_lock, fake_unlock, fake_free, &fake};
}

/// The table of `fake` for the test's length, none after it.
struct Installed {
    explicit Installed(Fake &fake) {
        const auto table = table_of(fake);
        CHECK(apkrt_set_hardware_buffers(&table) == 0);
    }
    ~Installed() { apkrt_set_hardware_buffers(nullptr); }
};

apkrt_hardware_buffer description(std::uint32_t width, std::uint32_t height, std::uint32_t format,
                                  std::uint64_t usage = cpu_read_often | cpu_write_often, std::uint32_t layers = 1) {
    apkrt_hardware_buffer buffer{};
    buffer.width = width;
    buffer.height = height;
    buffer.layers = layers;
    buffer.format = format;
    buffer.usage = usage;
    return buffer;
}

TEST(without_host_buffers_nothing_is_made) {
    apkrt_set_hardware_buffers(nullptr);
    auto buffer = description(64, 64, rgba8);
    CHECK(call(APKRT_HARDWARE_BUFFER_ALLOCATE, buffer) == -enosys);
    CHECK(call(APKRT_HARDWARE_BUFFER_SUPPORTED, buffer) == 0);
    CHECK(call(APKRT_HARDWARE_BUFFER_ACQUIRE, 1) == -einval);  // no such buffer
}

TEST(a_table_with_a_function_missing_is_refused) {
    Fake fake;
    auto table = table_of(fake);
    table.unlock = nullptr;
    CHECK(apkrt_set_hardware_buffers(&table) == -1);
    CHECK(apkrt_set_hardware_buffers(nullptr) == 0);
}

TEST(allocate_gives_ids_never_0_nor_reused_and_the_hosts_stride) {
    Fake fake;
    const Installed installed(fake);
    std::set<std::uint64_t> ids;
    for (int each = 0; each < 4; ++each) {
        auto buffer = description(100, 10, rgba8);
        buffer.id = 77;  // an ID in is no ID out
        CHECK(call(APKRT_HARDWARE_BUFFER_ALLOCATE, buffer) == 0);
        CHECK(buffer.id != 0 && buffer.id != 77 && buffer.stride == 112);
        ids.insert(buffer.id);
        CHECK(call(APKRT_HARDWARE_BUFFER_RELEASE, buffer.id) == 0);
    }
    CHECK(ids.size() == 4 && fake.allocations == 4 && fake.frees == 4);
}

TEST(the_runtime_refuses_what_could_harm_the_host) {
    Fake fake;
    const Installed installed(fake);
    for (auto buffer : {description(0, 8, rgba8), description(8, 0, rgba8), description(8, 8, rgba8, 0, 0),
                        description(8, 8, 0x99), description(8, 8, 0)}) {
        CHECK(call(APKRT_HARDWARE_BUFFER_ALLOCATE, buffer) == -einval);
        CHECK(call(APKRT_HARDWARE_BUFFER_SUPPORTED, buffer) == 0);
    }
    auto reserved = description(8, 8, rgba8);
    reserved.reserved = 1;
    CHECK(call(APKRT_HARDWARE_BUFFER_ALLOCATE, reserved) == -einval);
    CHECK(call(APKRT_HARDWARE_BUFFER_ALLOCATE, 0) == -efault);
    CHECK(call(APKRT_HARDWARE_BUFFER_LOCK, 0) == -efault);
    CHECK(call(0, 0) == -einval);
    CHECK(call(99, 0) == -einval);
    CHECK(fake.attempts == 0);
}

TEST(a_buffer_above_1_gib_never_reaches_the_host) {
    static_assert(APKRT_HARDWARE_BUFFER_MAX_BYTES == 1 << 30);
    Fake fake;
    fake.refuse = -static_cast<int>(enomem);  // nothing is really made
    const Installed installed(fake);
    // 1 GiB exactly reaches the host.
    auto most = description(16384, 16384, rgba8);
    CHECK(call(APKRT_HARDWARE_BUFFER_ALLOCATE, most) == -enomem);
    CHECK(fake.attempts == 1);
    for (auto buffer : {description(16385, 16384, rgba8), description(16384, 8193, rgba16f),
                        description(0xffffffff, 0xffffffff, rgba16f, 0, 0xffffffff),
                        description(0x10000, 0x10000, rgba8, 0, 0x10000), description(0x40000001, 1, blob),
                        description(4096, 4096, rgba8, 0, 65)}) {
        CHECK(call(APKRT_HARDWARE_BUFFER_ALLOCATE, buffer) == -enomem);
    }
    CHECK(fake.attempts == 1);
}

TEST(the_hosts_answers_come_back) {
    Fake fake;
    const Installed installed(fake);
    auto rgb = description(8, 8, rgb8);
    CHECK(call(APKRT_HARDWARE_BUFFER_SUPPORTED, rgb) == 0);
    CHECK(call(APKRT_HARDWARE_BUFFER_ALLOCATE, rgb) == -einval);
    auto ok = description(8, 8, rgb565);
    CHECK(call(APKRT_HARDWARE_BUFFER_SUPPORTED, ok) == 1);
    CHECK(fake.allocations == 0);
}

TEST(describe_answers_the_description_and_the_stride) {
    Fake fake;
    const Installed installed(fake);
    auto made = description(33, 7, rgba16f, cpu_read_often | gpu_sampled, 2);
    CHECK(call(APKRT_HARDWARE_BUFFER_ALLOCATE, made) == 0);
    apkrt_hardware_buffer asked{};
    asked.id = made.id;
    CHECK(call(APKRT_HARDWARE_BUFFER_DESCRIBE, asked) == 0);
    CHECK(asked.width == 33 && asked.height == 7 && asked.layers == 2 && asked.format == rgba16f &&
          asked.usage == (cpu_read_often | gpu_sampled) && asked.stride == 48);
    apkrt_hardware_buffer none{};
    none.id = made.id + 1000;
    CHECK(call(APKRT_HARDWARE_BUFFER_DESCRIBE, none) == -einval);
    none.id = 0;
    CHECK(call(APKRT_HARDWARE_BUFFER_DESCRIBE, none) == -einval);
    CHECK(call(APKRT_HARDWARE_BUFFER_RELEASE, made.id) == 0);
}

TEST(the_last_release_frees_and_the_id_is_gone) {
    Fake fake;
    const Installed installed(fake);
    auto made = description(16, 16, rgba8);
    CHECK(call(APKRT_HARDWARE_BUFFER_ALLOCATE, made) == 0);
    CHECK(call(APKRT_HARDWARE_BUFFER_ACQUIRE, made.id) == 0);
    CHECK(call(APKRT_HARDWARE_BUFFER_ACQUIRE, made.id) == 0);
    CHECK(call(APKRT_HARDWARE_BUFFER_RELEASE, made.id) == 0);
    CHECK(call(APKRT_HARDWARE_BUFFER_RELEASE, made.id) == 0);
    CHECK(fake.frees == 0);
    CHECK(call(APKRT_HARDWARE_BUFFER_RELEASE, made.id) == 0);
    CHECK(fake.frees == 1);
    for (const auto op : {APKRT_HARDWARE_BUFFER_ACQUIRE, APKRT_HARDWARE_BUFFER_RELEASE, APKRT_HARDWARE_BUFFER_UNLOCK}) {
        CHECK(call(op, made.id) == -einval);
    }
    CHECK(call(APKRT_HARDWARE_BUFFER_DESCRIBE, made) == -einval);
    CHECK(call(APKRT_HARDWARE_BUFFER_ACQUIRE, 0) == -einval);
}

TEST(the_host_gets_a_buffers_handle_table_and_description) {
    // ADR-0058 b): graphics/host turns the ID that the front libEGL passes into its own handle, and checks the table.
    Fake fake;
    const Installed installed(fake);
    auto made = description(20, 6, rgb565);
    CHECK(call(APKRT_HARDWARE_BUFFER_ALLOCATE, made) == 0);
    std::uint64_t handle = 0;
    void *context = nullptr;
    apkrt_hardware_buffer_description described{};
    std::uint32_t stride = 0;
    CHECK(apkrt_hardware_buffer_acquire_handle(made.id, &handle, &context, &described, &stride) == 0);
    CHECK(fake.buffers.size() == 1 && handle == fake.buffers.begin()->first && context == &fake);
    CHECK(described.width == 20 && described.height == 6 && described.layers == 1 && described.format == rgb565 &&
          described.usage == made.usage && stride == made.stride);
    CHECK(apkrt_hardware_buffer_acquire_handle(made.id, &handle, nullptr, nullptr, nullptr) == 0);  // the rest NULL
    apkrt_hardware_buffer_release_handle(made.id);
    apkrt_hardware_buffer_release_handle(made.id);
    CHECK(apkrt_hardware_buffer_acquire_handle(made.id + 1, &handle, nullptr, nullptr, nullptr) == -einval);
    CHECK(apkrt_hardware_buffer_acquire_handle(0, &handle, nullptr, nullptr, nullptr) == -einval);
    CHECK(apkrt_hardware_buffer_acquire_handle(made.id, nullptr, nullptr, nullptr, nullptr) == -einval);
    CHECK(fake.frees == 0);
    CHECK(call(APKRT_HARDWARE_BUFFER_RELEASE, made.id) == 0);
    CHECK(fake.frees == 1);
}

TEST(the_hosts_reference_outlives_the_guests_and_its_extra_releases) {
    // The guest's last release takes the ID away; the buffer stays the host's until its own release, and a release
    // too many of the guest's cannot free it under the host.
    Fake fake;
    const Installed installed(fake);
    auto made = description(8, 8, rgba8);
    CHECK(call(APKRT_HARDWARE_BUFFER_ALLOCATE, made) == 0);
    std::uint64_t handle = 0;
    CHECK(apkrt_hardware_buffer_acquire_handle(made.id, &handle, nullptr, nullptr, nullptr) == 0);
    CHECK(apkrt_hardware_buffer_acquire_handle(made.id, &handle, nullptr, nullptr, nullptr) == 0);
    CHECK(call(APKRT_HARDWARE_BUFFER_RELEASE, made.id) == 0);
    CHECK(call(APKRT_HARDWARE_BUFFER_RELEASE, made.id) == -einval);
    CHECK(call(APKRT_HARDWARE_BUFFER_DESCRIBE, made) == -einval);
    CHECK(fake.frees == 0 && fake.buffers.count(handle) == 1);
    apkrt_hardware_buffer_release_handle(made.id);
    CHECK(fake.frees == 0);
    apkrt_hardware_buffer_release_handle(made.id);
    CHECK(fake.frees == 1);
    apkrt_hardware_buffer_release_handle(made.id);  // none left: ignored
    CHECK(fake.frees == 1);
}

TEST(a_lock_gives_the_hosts_memory_until_its_unlock) {
    Fake fake;
    const Installed installed(fake);
    auto made = description(10, 4, rgba8);
    CHECK(call(APKRT_HARDWARE_BUFFER_ALLOCATE, made) == 0);
    apkrt_hardware_buffer lock{};
    lock.id = made.id;
    lock.usage = cpu_write_often;
    CHECK(call(APKRT_HARDWARE_BUFFER_LOCK, lock) == 0);
    CHECK(fake.buffers.size() == 1);
    if (fake.buffers.size() != 1 || lock.address == 0) return;
    CHECK(lock.address == reinterpret_cast<std::uint64_t>(fake.buffers.begin()->second.bytes.data()));
    CHECK(lock.bytes_per_pixel == 4 && lock.bytes_per_stride == 64);
    reinterpret_cast<unsigned char *>(lock.address)[63] = 0x5a;  // the guest writes there
    CHECK(call(APKRT_HARDWARE_BUFFER_UNLOCK, made.id) == 0);
    CHECK(call(APKRT_HARDWARE_BUFFER_UNLOCK, made.id) == -einval);  // no lock left
    CHECK(fake.buffers.begin()->second.bytes[63] == 0x5a);
    // Usage beyond the CPU's bits is no lock's.
    apkrt_hardware_buffer gpu{};
    gpu.id = made.id;
    gpu.usage = cpu_read_often | gpu_sampled;
    CHECK(call(APKRT_HARDWARE_BUFFER_LOCK, gpu) == -einval);
    gpu.usage = cpu_read_often;
    gpu.reserved = 1;
    CHECK(call(APKRT_HARDWARE_BUFFER_LOCK, gpu) == -einval);
    gpu.reserved = 0;
    gpu.id = made.id + 1000;
    CHECK(call(APKRT_HARDWARE_BUFFER_LOCK, gpu) == -einval);
    CHECK(fake.lock_calls == 1 && fake.unlock_calls == 1);
    CHECK(call(APKRT_HARDWARE_BUFFER_RELEASE, made.id) == 0);
}

TEST(locks_nest_and_a_release_while_locked_unlocks_first) {
    Fake fake;
    const Installed installed(fake);
    auto made = description(8, 8, r8);
    CHECK(call(APKRT_HARDWARE_BUFFER_ALLOCATE, made) == 0);
    apkrt_hardware_buffer lock{};
    lock.id = made.id;
    lock.usage = cpu_read_often;
    CHECK(call(APKRT_HARDWARE_BUFFER_LOCK, lock) == 0);
    CHECK(call(APKRT_HARDWARE_BUFFER_LOCK, lock) == 0);
    CHECK(call(APKRT_HARDWARE_BUFFER_UNLOCK, made.id) == 0);
    CHECK(fake.lock_calls == 2 && fake.unlock_calls == 1);
    CHECK(call(APKRT_HARDWARE_BUFFER_RELEASE, made.id) == 0);  // the free checks that no lock is left
    CHECK(fake.unlock_calls == 2 && fake.frees == 1);
}

TEST(a_buffer_keeps_the_table_that_made_it) {
    Fake first, second;
    auto made = description(8, 8, rgba8);
    {
        const Installed installed(first);
        CHECK(call(APKRT_HARDWARE_BUFFER_ALLOCATE, made) == 0);
    }
    const Installed installed(second);
    apkrt_hardware_buffer lock{};
    lock.id = made.id;
    lock.usage = cpu_read_often;
    CHECK(call(APKRT_HARDWARE_BUFFER_LOCK, lock) == 0 && call(APKRT_HARDWARE_BUFFER_UNLOCK, made.id) == 0);
    CHECK(call(APKRT_HARDWARE_BUFFER_RELEASE, made.id) == 0);
    CHECK(first.lock_calls == 1 && first.frees == 1 && second.lock_calls == 0 && second.frees == 0);
}

TEST(threads_get_ids_of_their_own) {
    Fake fake;
    const Installed installed(fake);
    std::vector<std::vector<std::uint64_t>> made(4);
    std::vector<std::thread> threads;
    for (auto &each : made) {
        threads.emplace_back([&each] {
            for (int count = 0; count < 50; ++count) {
                auto buffer = description(4, 4, rgba8);
                if (call(APKRT_HARDWARE_BUFFER_ALLOCATE, buffer) == 0) each.push_back(buffer.id);
            }
        });
    }
    for (auto &thread : threads) thread.join();
    std::set<std::uint64_t> all;
    for (const auto &each : made) all.insert(each.begin(), each.end());
    CHECK(all.size() == 200 && all.count(0) == 0);
    for (const auto id : all) CHECK(call(APKRT_HARDWARE_BUFFER_RELEASE, id) == 0);
    CHECK(fake.frees == 200);
}

// The reviewer's findings of 2026-10-08.

/// Whether `call` returns within two seconds; one that does not is left to run.
template <typename Call>
bool returns_soon(Call call) {
    auto done = std::async(std::launch::async, call);
    const bool soon = done.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    if (!soon) std::thread([done = std::move(done)]() mutable { done.wait(); }).detach();
    return soon;
}

TEST(a_lock_that_waits_in_the_host_holds_up_no_unlock) {
    Fake fake;
    const Installed installed(fake);
    auto made = description(8, 8, rgba8);
    CHECK(call(APKRT_HARDWARE_BUFFER_ALLOCATE, made) == 0);
    apkrt_hardware_buffer lock{};
    lock.id = made.id;
    lock.usage = cpu_write_often;
    CHECK(call(APKRT_HARDWARE_BUFFER_LOCK, lock) == 0);
    // A second lock waits in the host, as for the GPU's work on the buffer; the first one's unlock must not wait.
    std::promise<void> go;
    auto gate = go.get_future().share();
    fake.while_locking = [gate] { gate.wait(); };
    auto second = std::async(std::launch::async, [&] {
        apkrt_hardware_buffer again{};
        again.id = made.id;
        again.usage = cpu_read_often;
        return call(APKRT_HARDWARE_BUFFER_LOCK, again);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));  // the second lock is in the host by now
    CHECK(returns_soon([&] { return call(APKRT_HARDWARE_BUFFER_UNLOCK, made.id); }));
    go.set_value();
    CHECK(second.get() == 0);
    fake.while_locking = nullptr;
    CHECK(call(APKRT_HARDWARE_BUFFER_UNLOCK, made.id) == 0);
    CHECK(call(APKRT_HARDWARE_BUFFER_RELEASE, made.id) == 0 && fake.frees == 1);
}

TEST(the_cap_holds_where_the_products_wrap_to_0) {
    Fake fake;
    const Installed installed(fake);
    for (auto buffer : {description(0x80000000, 0x80000000, r8, 0, 4), description(0x80000000, 0x80000000, rgba8)}) {
        CHECK(call(APKRT_HARDWARE_BUFFER_ALLOCATE, buffer) == -enomem);
    }
    CHECK(fake.attempts == 0);
}

TEST(the_last_release_during_a_lock_frees_after_it) {
    Fake fake;
    const Installed installed(fake);
    auto made = description(8, 8, rgba8);
    CHECK(call(APKRT_HARDWARE_BUFFER_ALLOCATE, made) == 0);
    std::promise<void> go;
    auto gate = go.get_future().share();
    fake.while_locking = [gate] { gate.wait(); };
    auto locking = std::async(std::launch::async, [&] {
        apkrt_hardware_buffer lock{};
        lock.id = made.id;
        lock.usage = cpu_read_often;
        return call(APKRT_HARDWARE_BUFFER_LOCK, lock);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(call(APKRT_HARDWARE_BUFFER_RELEASE, made.id) == 0);
    CHECK(fake.frees == 0);  // the lock in the host still holds the buffer
    go.set_value();
    CHECK(locking.get() == 0);
    fake.while_locking = nullptr;
    CHECK(fake.unlock_calls == 1 && fake.frees == 1);  // its lock ended, then the free
    CHECK(call(APKRT_HARDWARE_BUFFER_UNLOCK, made.id) == -einval);
}

TEST(the_host_frees_under_no_lock_of_the_runtimes) {
    Fake fake;
    const Installed installed(fake);
    auto made = description(8, 8, rgba8);
    CHECK(call(APKRT_HARDWARE_BUFFER_ALLOCATE, made) == 0);
    fake.while_freeing = [&fake] {
        const auto table = table_of(fake);
        apkrt_set_hardware_buffers(&table);  // takes the registry's lock
    };
    CHECK(returns_soon([&] { return call(APKRT_HARDWARE_BUFFER_RELEASE, made.id); }));
    fake.while_freeing = nullptr;
    CHECK(fake.frees == 1);
}

TEST(a_refused_lock_or_unlock_keeps_the_count) {
    Fake fake;
    const Installed installed(fake);
    auto made = description(8, 8, rgba8);
    CHECK(call(APKRT_HARDWARE_BUFFER_ALLOCATE, made) == 0);
    apkrt_hardware_buffer lock{};
    lock.id = made.id;
    lock.usage = cpu_read_often;
    fake.refuse_lock = -16;  // EBUSY
    CHECK(call(APKRT_HARDWARE_BUFFER_LOCK, lock) == -16);
    CHECK(call(APKRT_HARDWARE_BUFFER_UNLOCK, made.id) == -einval);  // nothing counted
    fake.refuse_lock = 0;
    CHECK(call(APKRT_HARDWARE_BUFFER_LOCK, lock) == 0);
    fake.refuse_unlock = -16;
    CHECK(call(APKRT_HARDWARE_BUFFER_UNLOCK, made.id) == -16);
    fake.refuse_unlock = 0;
    CHECK(call(APKRT_HARDWARE_BUFFER_UNLOCK, made.id) == 0);  // the lock was still counted
    CHECK(call(APKRT_HARDWARE_BUFFER_RELEASE, made.id) == 0 && fake.frees == 1);
}

TEST(describe_checks_reserved_and_leaves_what_it_does_not_name) {
    Fake fake;
    const Installed installed(fake);
    auto made = description(8, 8, rgba8);
    CHECK(call(APKRT_HARDWARE_BUFFER_ALLOCATE, made) == 0);
    apkrt_hardware_buffer asked{};
    asked.id = made.id;
    asked.reserved = 1;
    CHECK(call(APKRT_HARDWARE_BUFFER_DESCRIBE, asked) == -einval);
    asked.reserved = 0;
    asked.address = 0x1234;
    asked.bytes_per_pixel = 77;
    CHECK(call(APKRT_HARDWARE_BUFFER_DESCRIBE, asked) == 0);
    CHECK(asked.address == 0x1234 && asked.bytes_per_pixel == 77 && asked.width == 8);
    CHECK(call(APKRT_HARDWARE_BUFFER_RELEASE, made.id) == 0);
}

TEST(the_vndks_formats_reach_the_host) {
    Fake fake;
    const Installed installed(fake);
    for (const std::uint32_t format : {0x22u /* IMPLEMENTATION_DEFINED */, 0x32315659u /* YV12 */,
                                       0x20203859u /* Y8 */, 5u /* B8G8R8A8 */}) {
        auto buffer = description(64, 64, format);
        CHECK(call(APKRT_HARDWARE_BUFFER_ALLOCATE, buffer) == -einval);  // the fake's answer
    }
    CHECK(fake.attempts == 4);
}

TEST(answers_of_the_host_beyond_errno_become_eio) {
    Fake fake;
    const Installed installed(fake);
    auto made = description(8, 8, rgba8);
    CHECK(call(APKRT_HARDWARE_BUFFER_ALLOCATE, made) == 0);
    apkrt_hardware_buffer lock{};
    lock.id = made.id;
    lock.usage = cpu_read_often;
    fake.refuse_lock = 5;
    CHECK(call(APKRT_HARDWARE_BUFFER_LOCK, lock) == -5);  // EIO
    fake.refuse_lock = -5000;
    CHECK(call(APKRT_HARDWARE_BUFFER_LOCK, lock) == -5);
    fake.refuse_lock = 0;
    fake.refuse = 1;
    auto other = description(8, 8, rgba8);
    CHECK(call(APKRT_HARDWARE_BUFFER_ALLOCATE, other) == -5);
    CHECK(call(APKRT_HARDWARE_BUFFER_RELEASE, made.id) == 0);
}

TEST(supported_is_1_only_for_the_hosts_1) {
    // As AHardwareBuffer_isSupported answers: 1 or 0, and a host's error or other answer is not a yes.
    Fake fake;
    const Installed installed(fake);
    auto asked = description(8, 8, rgba8);
    for (const int answer : {-12, -1, 2, 7}) {
        fake.supported_answer = answer;
        CHECK(call(APKRT_HARDWARE_BUFFER_SUPPORTED, asked) == 0);
    }
    fake.supported_answer = 1;
    CHECK(call(APKRT_HARDWARE_BUFFER_SUPPORTED, asked) == 1);
}

TEST(a_struct_the_guest_cannot_read_or_write_faults_and_leaves_nothing) {
    Fake fake;
    const Installed installed(fake);
    const auto page = static_cast<std::size_t>(getpagesize());
    auto *memory = static_cast<unsigned char *>(::mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE,
                                                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    CHECK(memory != MAP_FAILED);
    CHECK(::munmap(memory + page, page) == 0);
    const auto in_hole = reinterpret_cast<std::uint64_t>(memory + page - 8);  // its last 48 bytes in the hole
    for (const auto op : {APKRT_HARDWARE_BUFFER_ALLOCATE, APKRT_HARDWARE_BUFFER_SUPPORTED,
                          APKRT_HARDWARE_BUFFER_DESCRIBE, APKRT_HARDWARE_BUFFER_LOCK}) {
        CHECK(call(op, in_hole) == -efault);
    }
    CHECK(call(APKRT_HARDWARE_BUFFER_ALLOCATE, 5) == -efault);  // an ID where a struct belongs
    // Read-only: the answer cannot go back, so what was made or locked goes again.
    auto made = description(8, 8, rgba8);
    std::memcpy(memory, &made, sizeof made);
    CHECK(::mprotect(memory, page, PROT_READ) == 0);
    CHECK(call(APKRT_HARDWARE_BUFFER_ALLOCATE, reinterpret_cast<std::uint64_t>(memory)) == -efault);
    CHECK(fake.allocations == 1 && fake.frees == 1);
    CHECK(call(APKRT_HARDWARE_BUFFER_ALLOCATE, made) == 0);
    CHECK(::mprotect(memory, page, PROT_READ | PROT_WRITE) == 0);
    apkrt_hardware_buffer lock{};
    lock.id = made.id;
    lock.usage = cpu_read_often;
    std::memcpy(memory, &lock, sizeof lock);
    CHECK(::mprotect(memory, page, PROT_READ) == 0);
    CHECK(call(APKRT_HARDWARE_BUFFER_LOCK, reinterpret_cast<std::uint64_t>(memory)) == -efault);
    CHECK(fake.lock_calls == 1 && fake.unlock_calls == 1);
    CHECK(call(APKRT_HARDWARE_BUFFER_UNLOCK, made.id) == -einval);
    CHECK(call(APKRT_HARDWARE_BUFFER_RELEASE, made.id) == 0);
    ::munmap(memory, page);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // each test's line before a crash in the next
    return apk::testing::run_all();
}
