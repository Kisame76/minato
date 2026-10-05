// Checks guest threads (M2 step 5c): clone as bionic's pthread_create calls it, the state a child
// starts in (the parent's registers at the svc, its own stack and thread pointer, its thread ID),
// and how it ends: exit with CLONE_CHILD_CLEARTID wakes a joiner, and a thread may unmap its own
// stack right before exit, as bionic's detached threads do. Everything goes through the syscall
// entry exactly as converted guest code calls it (threads_probe.S).
#include "APKRuntime/apkrt.h"
#include "syscalls.h"
#include "threads.h"

#include "check.h"

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <set>
#include <string_view>
#include <thread>

extern "C" {

struct ProbeRegisters {
    std::uint64_t x[10];  // x19-x28
    __uint128_t q[8];     // v8-v15
    std::uint64_t sp;
};
static_assert(offsetof(ProbeRegisters, q) == 80 && offsetof(ProbeRegisters, sp) == 208, "threads_probe.S");

ProbeRegisters probe_marks;
ProbeRegisters probe_child_registers;

long probe_clone(std::uint64_t flags, void *child_stack, int *parent_tid, void *tls, int *child_tid,
                 void (*function)(void *), void *argument);
long probe_syscall(std::uint64_t number, std::uint64_t a0, std::uint64_t a1 = 0, std::uint64_t a2 = 0,
                   std::uint64_t a3 = 0);
}

namespace {

// Linux: include/uapi/linux/sched.h, asm-generic/unistd.h, errno.h.
constexpr std::uint64_t clone_vm = 0x100, clone_fs = 0x200, clone_files = 0x400, clone_sighand = 0x800,
                        clone_vfork = 0x4000, clone_thread = 0x10000, clone_sysvsem = 0x40000,
                        clone_settls = 0x80000, clone_parent_settid = 0x100000,
                        clone_child_cleartid = 0x200000, clone_child_settid = 0x1000000;
constexpr std::uint64_t linux_sigchld = 17;
/// What bionic's pthread_create passes.
constexpr std::uint64_t pthread_flags = clone_vm | clone_fs | clone_files | clone_sighand | clone_thread |
                                        clone_sysvsem | clone_settls | clone_parent_settid | clone_child_cleartid;
constexpr std::uint64_t nr_exit = 93, nr_futex = 98, nr_set_tid_address = 96, nr_gettid = 178, nr_munmap = 215,
                        nr_mremap = 216;
constexpr std::uint64_t mremap_maymove = 1;
constexpr std::uint64_t futex_wait = 0, futex_wake = 1;
constexpr long enosys = 38, eagain = 11;

constexpr std::size_t stack_size = 256 * 1024;

void *map_stack() {
    void *stack = ::mmap(nullptr, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return stack == MAP_FAILED ? nullptr : stack;
}

void *top_of(void *stack) {
    return static_cast<char *>(stack) + stack_size;
}

bool mapped(void *stack) {
    return ::msync(stack, stack_size, MS_ASYNC) == 0;  // ENOMEM once unmapped
}

/// pthread_join's wait: until the kernel clears the thread ID word at the thread's exit.
void join(int *tid_word) {
    for (int value; (value = __atomic_load_n(tid_word, __ATOMIC_ACQUIRE)) != 0;) {
        const long result = probe_syscall(nr_futex, reinterpret_cast<std::uint64_t>(tid_word), futex_wait,
                                          static_cast<std::uint64_t>(value), 0);
        CHECK(result == 0 || result == -eagain);
    }
}

struct ChildView {
    std::atomic<int> go{0};
    void *tls = nullptr;
    long tid = 0;
    int child_tid_word = 0;
    void *argument = nullptr;
};

void child_main(void *argument) {
    auto &view = *static_cast<ChildView *>(argument);
    view.argument = argument;
    view.tls = apkrt_get_guest_tls();
    view.tid = probe_syscall(nr_gettid, 0, 0, 0);
    while (view.go.load() == 0) std::this_thread::yield();
}

TEST(clone_starts_a_thread_the_way_pthread_create_asks_for) {
    for (int i = 0; i < 10; ++i) probe_marks.x[i] = 0x1900'0000'0000'0000ull + 0x0101'0101ull * static_cast<unsigned>(i);
    for (int i = 0; i < 8; ++i) probe_marks.q[i] = (static_cast<__uint128_t>(0xabcd'0000u + static_cast<unsigned>(i)) << 64) | (0x8000u + static_cast<unsigned>(i));
    std::memset(&probe_child_registers, 0, sizeof probe_child_registers);

    void *stack = map_stack();
    CHECK(stack != nullptr);
    alignas(16) static std::uint64_t tls[16];
    static ChildView view;
    int thread_tid = 0;  // bionic's pthread_internal_t::tid: set by the parent, cleared at the child's exit
    const long tid = probe_clone(pthread_flags | clone_child_settid, top_of(stack), &thread_tid, tls, &thread_tid,
                                 child_main, &view);
    CHECK(tid > 0 && tid != ::getpid());
    CHECK(thread_tid == tid);  // CLONE_PARENT_SETTID, before clone returned to the parent
    while (view.tid == 0) std::this_thread::yield();
    CHECK(view.argument == &view);
    CHECK(view.tls == tls);  // CLONE_SETTLS
    CHECK(view.tid == tid);  // gettid in the child
    for (int i = 0; i < 10; ++i) CHECK(probe_child_registers.x[i] == probe_marks.x[i]);
    for (int i = 0; i < 8; ++i) CHECK(probe_child_registers.q[i] == probe_marks.q[i]);
    CHECK(probe_child_registers.sp == reinterpret_cast<std::uint64_t>(top_of(stack)) - 16);
    view.go = 1;
    join(&thread_tid);  // CLONE_CHILD_CLEARTID and the futex wake at exit
    CHECK(thread_tid == 0);
    CHECK(apkrt_get_guest_tls() == nullptr);  // the parent's own thread pointer is untouched
    ::munmap(stack, stack_size);
}

struct Counter {
    std::atomic<int> started{0};
    std::atomic<int> sum{0};
};

void count_once(void *argument) {
    auto &counter = *static_cast<Counter *>(argument);
    counter.sum += static_cast<int>(probe_syscall(nr_gettid, 0, 0, 0) != 0);
    ++counter.started;
}

TEST(many_threads_get_their_own_ids_and_all_end) {
    constexpr int count = 16;
    static Counter counter;
    void *stacks[count];
    int tid_words[count];
    std::set<long> tids;
    for (int i = 0; i < count; ++i) {
        stacks[i] = map_stack();
        tid_words[i] = 0;
        const long tid = probe_clone(pthread_flags, top_of(stacks[i]), &tid_words[i], nullptr, &tid_words[i],
                                     count_once, &counter);
        CHECK(tid > 0);
        tids.insert(tid);
    }
    CHECK(tids.size() == count);
    for (int i = 0; i < count; ++i) join(&tid_words[i]);
    CHECK(counter.started == count && counter.sum == count);
    for (void *stack : stacks) ::munmap(stack, stack_size);
}

struct Teardown {
    void *stack;
    std::atomic<int> unmapped{0};
};

void unmap_own_stack(void *argument) {
    auto &teardown = *static_cast<Teardown *>(argument);
    // As bionic's pthread_exit for a detached thread: no tid to clear, then _exit_with_stack_teardown.
    probe_syscall(nr_set_tid_address, 0, 0, 0);
    teardown.unmapped = static_cast<int>(
        probe_syscall(nr_munmap, reinterpret_cast<std::uint64_t>(teardown.stack), stack_size, 0) == 0);
}

TEST(a_thread_may_unmap_its_own_stack_right_before_exit) {
    static Teardown teardown;
    teardown.stack = map_stack();
    int tid_word = 0;
    CHECK(probe_clone(pthread_flags, top_of(teardown.stack), &tid_word, nullptr, &tid_word, unmap_own_stack,
                      &teardown) > 0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (mapped(teardown.stack) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(teardown.unmapped == 1);
    CHECK(!mapped(teardown.stack));  // gone once the thread is off it
    CHECK(tid_word != 0);            // set_tid_address(NULL): nobody clears it
}

/// Linux's mmap_lock (ADR-0021): the unmap a thread deferred to its exit waits while an mremap has looked at the VM
/// map and not yet changed it (sys::memory::test_pause holds the mremap between the two).
namespace paused_mremap {
Teardown teardown;
std::atomic<bool> go{false};

void unmap_own_stack_then_wait(void *) {
    probe_syscall(nr_set_tid_address, 0, 0, 0);
    teardown.unmapped = static_cast<int>(
        probe_syscall(nr_munmap, reinterpret_cast<std::uint64_t>(teardown.stack), stack_size, 0) == 0);
    while (!go.load()) std::this_thread::yield();
}

bool waited = false;

/// Lets the thread exit and gives its unmap up to 200 ms.
void let_the_thread_exit() {
    apkrt::sys::memory::test_pause = nullptr;  // once
    go = true;
    for (int i = 0; i < 200 && mapped(teardown.stack); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    waited = mapped(teardown.stack);
}

}  // namespace paused_mremap

TEST(a_threads_exit_unmaps_its_stack_only_after_an_mremap_that_has_looked) {
    using namespace paused_mremap;
    teardown.stack = map_stack();
    int tid_word = 0;
    CHECK(probe_clone(pthread_flags, top_of(teardown.stack), &tid_word, nullptr, &tid_word, unmap_own_stack_then_wait,
                      nullptr) > 0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (teardown.unmapped == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    CHECK(teardown.unmapped == 1 && mapped(teardown.stack));  // deferred
    // An mremap that grows the first page of two, which moves it.
    const auto page = static_cast<std::uint64_t>(::getpagesize());
    void *const range = ::mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    apkrt::sys::memory::test_pause = let_the_thread_exit;
    const long moved = probe_syscall(nr_mremap, reinterpret_cast<std::uint64_t>(range), page, 2 * page, mremap_maymove);
    apkrt::sys::memory::test_pause = nullptr;
    go = true;
    CHECK(waited);
    while (mapped(teardown.stack) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(!mapped(teardown.stack));  // once the mremap is done
    CHECK(moved > 0);
    ::munmap(reinterpret_cast<void *>(moved), 2 * page);
    ::munmap(range, 2 * page);
}

TEST(fork_and_vfork_are_refused) {
    // iOS has no fork (F11); Linux never answers ENOSYS for clone, so programs see a clear failure.
    void *stack = map_stack();  // the probe pushes onto it before the syscall
    CHECK(probe_clone(linux_sigchld, top_of(stack), nullptr, nullptr, nullptr, nullptr, nullptr) == -enosys);
    CHECK(probe_clone(clone_vm | clone_vfork | linux_sigchld, top_of(stack), nullptr, nullptr, nullptr, nullptr,
                      nullptr) == -enosys);
    ::munmap(stack, stack_size);
}

TEST(futex_wake_reports_how_many_waited) {
    int word = 0;
    CHECK(probe_syscall(nr_futex, reinterpret_cast<std::uint64_t>(&word), futex_wake, 1) == 0);
}

// ADR-0020 X4: the islands keep the guest's x18 in TSD slot 11. On Linux a clone child starts with
// all of the parent's registers, x18 included.
TEST(clone_hands_the_parents_x18_to_the_child) {
    static std::atomic<std::uint64_t> seen{0};
    apkrt_set_guest_x18(0x1818'1818'1818'1818);
    void *stack = map_stack();
    CHECK(stack != nullptr);
    int tid_word = 0;
    const long tid = probe_clone(pthread_flags, top_of(stack), &tid_word, nullptr, &tid_word,
                                 [](void *) { seen.store(apkrt_get_guest_x18()); }, nullptr);
    CHECK(tid > 0);
    join(&tid_word);
    CHECK(seen.load() == 0x1818'1818'1818'1818);
    CHECK(apkrt_get_guest_x18() == 0x1818'1818'1818'1818);  // the parent's own is untouched
    apkrt_set_guest_x18(0);
    ::munmap(stack, stack_size);
}

// A guest thread finds its thread pointer in slot 6 and its x18 in slot 11 when it starts, and
// leaves both empty on its host thread when it ends.
TEST(a_guest_thread_starts_with_its_x18_and_leaves_both_slots_empty) {
    std::thread([] {
        static apkrt::threads::GuestThread thread;
        static std::uint64_t x18 = 0;
        static void *thread_pointer = nullptr;
        thread.tid = apkrt::threads::new_tid();
        thread.thread_pointer = 0x7000;
        thread.x18 = 0x1818;
        apkrt::threads::add(thread);
        apkrt::threads::run(
            thread,
            [](void *) {
                x18 = apkrt_get_guest_x18();
                thread_pointer = apkrt_get_guest_tls();
                probe_syscall(nr_exit, 0);  // back into run(), which ends the guest thread
            },
            nullptr);
        CHECK(x18 == 0x1818);
        CHECK(thread_pointer == reinterpret_cast<void *>(0x7000));
        CHECK(apkrt_get_guest_x18() == 0);
        CHECK(apkrt_get_guest_tls() == nullptr);
    }).join();
}

// The program's first thread needs both slots empty; the runtime refuses to start it otherwise.
TEST(the_program_thread_needs_both_slots_empty) {
    std::thread([] {
        const auto used = [] {
            const char *slot = apkrt::threads::used_guest_slot();
            return std::string_view(slot != nullptr ? slot : "none");
        };
        CHECK(used() == "none");
        apkrt_set_guest_x18(1);
        CHECK(used() == "Darwin TSD slot 11");
        apkrt_set_guest_x18(0);
        apkrt_set_guest_tls(reinterpret_cast<void *>(0x7000));
        CHECK(used() == "Darwin TSD slot 6");
        apkrt_set_guest_tls(nullptr);
        CHECK(used() == "none");
    }).join();
}

}  // namespace

int main() {
    // The test's own thread is the program's first thread: the process lives as long as it does.
    static apkrt::threads::GuestThread first{};
    first.tid = ::getpid();
    apkrt::threads::add(first);
    return apk::testing::run_all();
}
