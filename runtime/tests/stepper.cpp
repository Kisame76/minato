#include "stepper.h"

#include <mach/mach.h>
#include <mach/thread_status.h>
#include <pthread.h>

#include <atomic>
#include <cstring>
#include <stdexcept>
#include <string>

extern "C" {
std::uint64_t stepper_call(std::uint64_t (*function)(std::uint64_t), std::uint64_t argument);
extern const std::uint32_t stepper_call_brk[];
extern const std::uint32_t stepper_call_return[];
}

namespace apkrt::testing {
namespace {

// mach_exception_raise (EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES) and its reply, as MIG lays them out.
#pragma pack(push, 4)
struct ExceptionRequest {
    mach_msg_header_t head;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t thread;
    mach_msg_port_descriptor_t task;
    NDR_record_t ndr;
    exception_type_t exception;
    mach_msg_type_number_t code_count;
    std::int64_t code[2];
    char trailer[128];
};
struct ExceptionReply {
    mach_msg_header_t head;
    NDR_record_t ndr;
    kern_return_t result;
};
#pragma pack(pop)

constexpr std::uint64_t single_step = 1;           // MDSCR_EL1.SS
constexpr std::uint64_t breakpoint_control = 0x1e5;  // DBGBCR: enabled, at EL0, all four bytes of the word
constexpr std::size_t step_limit = 200'000;          // a stepped call that never returns
constexpr mach_msg_timeout_t wait_ms = 10'000;

void check(kern_return_t result, const char *what) {
    if (result != KERN_SUCCESS) throw std::runtime_error(std::string(what) + ": " + mach_error_string(result));
}

arm_debug_state64_t debug_state(thread_t thread) {
    arm_debug_state64_t state{};
    mach_msg_type_number_t count = ARM_DEBUG_STATE64_COUNT;
    check(thread_get_state(thread, ARM_DEBUG_STATE64, reinterpret_cast<thread_state_t>(&state), &count),
          "thread_get_state(ARM_DEBUG_STATE64)");
    return state;
}

void set_debug_state(thread_t thread, arm_debug_state64_t state) {
    check(thread_set_state(thread, ARM_DEBUG_STATE64, reinterpret_cast<thread_state_t>(&state),
                           ARM_DEBUG_STATE64_COUNT),
          "thread_set_state(ARM_DEBUG_STATE64)");
}

struct Stepped {
    std::uint64_t (*function)(std::uint64_t);
    std::uint64_t argument;
    mach_port_t port;
    std::atomic<const std::uint64_t *> tsd{nullptr};  // its Darwin TSD, for slot 11
    std::uint64_t result = 0;
    std::atomic<bool> ended{false};
};

void *run_stepped(void *raw) {
    auto &stepped = *static_cast<Stepped *>(raw);
    std::uint64_t base;
    __asm__ volatile("mrs %0, TPIDRRO_EL0" : "=r"(base));
    stepped.tsd = reinterpret_cast<const std::uint64_t *>(base & ~std::uint64_t{7});
    const thread_t self = mach_thread_self();
    const auto ports = thread_set_exception_ports(self, EXC_MASK_BREAKPOINT, stepped.port,
                                                  static_cast<exception_behavior_t>(EXCEPTION_DEFAULT |
                                                                                    MACH_EXCEPTION_CODES),
                                                  ARM_THREAD_STATE64);
    mach_port_deallocate(mach_task_self(), self);
    // Without the port, the BRK would end the process.
    if (ports == KERN_SUCCESS) stepped.result = stepper_call(stepped.function, stepped.argument);
    stepped.ended = true;
    return nullptr;
}

void reply(const ExceptionRequest &request) {
    ExceptionReply answer{};
    answer.head.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(request.head.msgh_bits), 0);
    answer.head.msgh_remote_port = request.head.msgh_remote_port;
    answer.head.msgh_local_port = MACH_PORT_NULL;
    answer.head.msgh_id = request.head.msgh_id + 100;
    answer.head.msgh_size = sizeof answer;
    answer.ndr = NDR_record;
    answer.result = KERN_SUCCESS;
    check(mach_msg(&answer.head, MACH_SEND_MSG, sizeof answer, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE,
                   MACH_PORT_NULL),
          "mach_msg(reply)");
}

}  // namespace

std::vector<Step> step_through(std::uint64_t (*function)(std::uint64_t), std::uint64_t argument,
                               const std::vector<FreeRun> &free_runs, std::uint64_t &result,
                               const std::function<void(const Step &)> &visit) {
    mach_port_t port = MACH_PORT_NULL;
    check(mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port), "mach_port_allocate");
    check(mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND), "mach_port_insert_right");
    Stepped stepped{.function = function, .argument = argument, .port = port};
    pthread_t thread;
    if (pthread_create(&thread, nullptr, run_stepped, &stepped) != 0) throw std::runtime_error("pthread_create");

    const auto brk = reinterpret_cast<std::uint64_t>(stepper_call_brk);
    const auto returned = reinterpret_cast<std::uint64_t>(stepper_call_return);
    std::vector<unsigned> visits(free_runs.size(), 0);
    std::vector<Step> steps;
    std::string failure;
    bool lost = false;
    std::uint64_t free_run_to = 0;  // where the free run in progress ends, or 0
    for (bool stepping = true; stepping;) {
        ExceptionRequest request{};
        const auto received = mach_msg(&request.head, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof request, port,
                                       wait_ms, MACH_PORT_NULL);
        if (received != MACH_MSG_SUCCESS) {
            failure = std::string("no exception from the stepped thread: ") + mach_error_string(received);
            break;
        }
        const thread_t target = request.thread.name;
        arm_thread_state64_t state{};
        mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
        check(thread_get_state(target, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state), &count),
              "thread_get_state(ARM_THREAD_STATE64)");
        const std::uint64_t pc = arm_thread_state64_get_pc(state);
        const bool breakpoint = request.code[1] != 0;  // a hardware breakpoint reports its address, a step 0
        auto debug = debug_state(target);
        // Breakpoint 1 stays at the return in every state the thread gets: no state, not even a stale
        // one (LostSteps), lets it return unnoticed.
        debug.__bvr[1] = returned;
        debug.__bcr[1] = breakpoint_control;
        if (pc == brk) {
            // The start: on past the BRK, one step at a time.
            arm_thread_state64_set_pc_fptr(state, reinterpret_cast<void *>(pc + 4));
            check(thread_set_state(target, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state), count),
                  "thread_set_state(ARM_THREAD_STATE64)");
            debug.__mdscr_el1 |= single_step;
            debug.__bcr[0] = 0;
        } else if ((breakpoint && pc != free_run_to) || pc == returned || steps.size() == step_limit) {
            // A breakpoint that no free run set up: the thread got there without steps.
            lost = breakpoint && pc != free_run_to;
            if (!lost && pc != returned) failure = "the stepped call did not return";
            debug.__mdscr_el1 &= ~single_step;
            debug.__bcr[0] = 0;
            debug.__bcr[1] = 0;
            stepping = false;
        } else {
            Step step{.state = state, .red_zone = {}, .x18 = stepped.tsd.load()[11]};
            const auto sp = arm_thread_state64_get_sp(state);
            std::memcpy(step.red_zone, reinterpret_cast<const void *>(sp - 128), sizeof step.red_zone);
            steps.push_back(step);
            if (visit) visit(step);
            debug.__mdscr_el1 |= single_step;
            debug.__bcr[0] = 0;
            free_run_to = 0;
            for (std::size_t index = 0; index < free_runs.size(); ++index) {
                if (pc != free_runs[index].from || visits[index]++ < free_runs[index].after_visits) continue;
                debug.__mdscr_el1 &= ~single_step;
                debug.__bvr[0] = free_runs[index].to;
                debug.__bcr[0] = breakpoint_control;
                free_run_to = free_runs[index].to;
            }
        }
        set_debug_state(target, debug);
        reply(request);
        mach_port_deallocate(mach_task_self(), request.thread.name);
        mach_port_deallocate(mach_task_self(), request.task.name);
    }
    pthread_join(thread, nullptr);
    mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1);
    if (lost) throw LostSteps("the stepped thread ran without single steps");
    if (!failure.empty()) throw std::runtime_error(failure);
    result = stepped.result;
    return steps;
}

}  // namespace apkrt::testing
