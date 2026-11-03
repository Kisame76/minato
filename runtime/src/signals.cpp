#include "signals.h"

#include "APKRuntime/apkrt.h"
#include "islands.h"
#include "process_exit.h"
#include "sockets.h"
#include "threads.h"

#include <fcntl.h>
#include <mach/mach.h>
#include <os/os_sync_wait_on_address.h>
#include <signal.h>
#include <sys/event.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include <cstddef>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>

namespace apkrt::signals {
namespace {

// sa_flags (include/uapi/asm-generic/signal.h).
constexpr std::uint64_t sa_siginfo = 0x4;
constexpr std::uint64_t sa_onstack = 0x0800'0000;
constexpr std::uint64_t sa_restart = 0x1000'0000;
constexpr std::uint64_t sa_nodefer = 0x4000'0000;
constexpr std::uint64_t sa_resethand = 0x8000'0000;
constexpr std::uint64_t sa_restorer = 0x0400'0000;
constexpr std::uint64_t sig_dfl = 0;
constexpr std::uint64_t sig_ign = 1;

// Linux signal numbers used here.
constexpr int sigill = 4, sigtrap = 5, sigbus = 7, sigfpe = 8, sigkill = 9, sigsegv = 11, sigchld = 17,
              sigcont = 18, sigstop = 19, sigtstp = 20, sigttin = 21, sigttou = 22, sigurg = 23, sigwinch = 28,
              sigsys = 31;
constexpr std::uint64_t unblockable = bit(sigkill) | bit(sigstop);
constexpr std::uint64_t synchronous = bit(sigsegv) | bit(sigbus) | bit(sigill) | bit(sigtrap) | bit(sigfpe) | bit(sigsys);

// The Linux rt_sigframe on arm64 (arch/arm64/kernel/signal.c): siginfo, then the ucontext with
// struct sigcontext, whose __reserved area holds the FPSIMD and ESR records.
struct LinuxUcontext {
    std::uint64_t flags;
    std::uint64_t link;
    LinuxStack stack;
    std::uint64_t sigmask;
    std::uint8_t unused[120];  // sigset_t has room for 1024 signals
    std::uint64_t padding;
    std::uint64_t fault_address;  // struct sigcontext, 16-byte aligned
    std::uint64_t regs[31];
    std::uint64_t sp;
    std::uint64_t pc;
    std::uint64_t pstate;
    std::uint64_t padding2;
    std::uint8_t reserved[4096];
};
static_assert(offsetof(LinuxUcontext, fault_address) == 176 && offsetof(LinuxUcontext, reserved) == 464);
static_assert(sizeof(LinuxUcontext) == 4560);

struct LinuxSigframe {
    LinuxSiginfo info;
    LinuxUcontext uc;
};
static_assert(sizeof(LinuxSigframe) == 4688 && sizeof(LinuxSigframe) % 16 == 0);

constexpr std::uint32_t fpsimd_magic = 0x4650'8001;
constexpr std::uint32_t esr_magic = 0x4553'5201;

struct RecordHeader {
    std::uint32_t magic;
    std::uint32_t size;
};

struct FpsimdRecord {
    RecordHeader head;
    std::uint32_t fpsr;
    std::uint32_t fpcr;
    __uint128_t v[32];
};
static_assert(sizeof(FpsimdRecord) == 528);

struct EsrRecord {
    RecordHeader head;
    std::uint64_t esr;
};

/// What a fault reports beyond siginfo: the fault address and the exception syndrome.
struct Fault {
    std::uint64_t address;
    std::uint64_t esr;
};

// --- Dispositions ------------------------------------------------------------------------------------
//
// Changed by rt_sigaction on any thread, read by signal handlers: a spin lock, which a handler can
// wait for, because the thread it interrupted never holds it (that thread is in the runtime then,
// and handlers leave such a thread alone).

std::atomic_flag actions_lock = ATOMIC_FLAG_INIT;
LinuxSigaction actions[signal_count + 1] = {};
std::atomic<std::uint64_t> default_restorer{0};

// Linux's shared_pending: a signal sent to the process that every thread blocks waits here, for a thread's
// sigtimedwait (take) or for the first thread that unblocks it (claim_process_signals). Senders hold its lock
// while they look at the masks, and a thread looks here after it changed its mask: none of them misses the other.
ThreadSignals process_signals;

class SpinLock {
public:
    explicit SpinLock(std::atomic_flag &flag) : flag_(flag) {
        while (flag_.test_and_set(std::memory_order_acquire)) {
        }
    }
    ~SpinLock() { flag_.clear(std::memory_order_release); }
    SpinLock(const SpinLock &) = delete;
    SpinLock &operator=(const SpinLock &) = delete;

private:
    std::atomic_flag &flag_;
};

/// Linux's default actions: these do nothing, the stop signals do nothing here either (no job
/// control on iOS), everything else ends the process.
bool ignored_by_default(int signal) {
    return signal == sigchld || signal == sigcont || signal == sigurg || signal == sigwinch || signal == sigstop ||
           signal == sigtstp || signal == sigttin || signal == sigttou;
}

bool ignored(const LinuxSigaction &action, int signal) {
    return action.handler == sig_ign || (action.handler == sig_dfl && ignored_by_default(signal));
}

// --- Host side --------------------------------------------------------------------------------------

/// Darwin's number for a Linux signal, or 0 when Darwin has no such signal.
int to_darwin_signal(int signal) {
    switch (signal) {
        case 1: return SIGHUP;
        case 2: return SIGINT;
        case 3: return SIGQUIT;
        case 4: return SIGILL;
        case 5: return SIGTRAP;
        case 6: return SIGABRT;
        case 7: return SIGBUS;
        case 8: return SIGFPE;
        case 9: return SIGKILL;
        case 10: return SIGUSR1;
        case 11: return SIGSEGV;
        case 12: return SIGUSR2;
        case 13: return SIGPIPE;
        case 14: return SIGALRM;
        case 15: return SIGTERM;
        case 24: return SIGXCPU;
        case 25: return SIGXFSZ;
        case 26: return SIGVTALRM;
        case 27: return SIGPROF;
        case 29: return SIGIO;
        case 31: return SIGSYS;
        default: return 0;  // no host signal, or one whose default action is not to end the process
    }
}

/// Ends the process by `signal`, as Linux does when its default action applies: by the host's
/// signal of the same meaning, else with the shell's exit status for it. From a signal handler: the
/// raised signal arrives when the handler returns.
void die_by(int signal) {
    const int darwin = to_darwin_signal(signal);
    if (darwin == 0) process::exit(128 + signal);
    process::report_death_by(signal);
    struct sigaction default_action {};
    default_action.sa_handler = SIG_DFL;
    ::sigaction(darwin, &default_action, nullptr);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, darwin);
    ::pthread_sigmask(SIG_UNBLOCK, &set, nullptr);
    ::pthread_kill(::pthread_self(), darwin);
}

std::once_flag installed;
pthread_key_t request_key;  // the context a thread asked to resume, until its signal arrives
constexpr int fault_signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE};
struct sigaction previous_fault_actions[std::size(fault_signals)];

thread_local ThreadSignals host_thread_signals;  // for threads without a guest thread (tests)

// --- Pending signals -----------------------------------------------------------------------------

bool is_real_time(int signal) {
    return signal >= 32;
}

/// Adds a signal to the thread's pending ones. Standard signals do not queue; real-time ones do,
/// up to the queue's size (then EAGAIN, as Linux answers at its RLIMIT_SIGPENDING).
long enqueue_locked(ThreadSignals &state, int signal, const LinuxSiginfo &info) {
    if (is_real_time(signal)) {
        if (state.queued == ThreadSignals::queue_capacity) return -11;  // EAGAIN
        state.queue[state.queued++] = {signal, info};
    } else if ((state.pending.load() & bit(signal)) == 0) {
        state.info[signal] = info;
    }
    state.pending.fetch_or(bit(signal));
    return 0;
}

long enqueue(ThreadSignals &state, int signal, const LinuxSiginfo &info) {
    const SpinLock lock(state.lock);
    return enqueue_locked(state, signal, info);
}

/// The next signal among `allowed` as Linux picks it: synchronous ones first, then the lowest.
int next_signal(std::uint64_t candidates) {
    if (candidates == 0) return 0;
    const auto first = (candidates & synchronous) != 0 ? (candidates & synchronous) : candidates;
    return __builtin_ctzll(first) + 1;
}

bool take_locked(ThreadSignals &state, std::uint64_t allowed, int &signal, LinuxSiginfo &info) {
    signal = next_signal(state.pending.load() & allowed);
    if (signal == 0) return false;
    if (!is_real_time(signal)) {
        info = state.info[signal];
        state.pending.fetch_and(~bit(signal));
        return true;
    }
    int index = 0;
    while (index < state.queued && state.queue[index].signal != signal) ++index;
    if (index == state.queued) {  // cannot happen: the bit is set only while the queue has one
        state.pending.fetch_and(~bit(signal));
        return false;
    }
    info = state.queue[index].info;
    for (int next = index + 1; next < state.queued; ++next) state.queue[next - 1] = state.queue[next];
    --state.queued;
    bool more = false;
    for (int next = 0; next < state.queued; ++next) more = more || state.queue[next].signal == signal;
    if (!more) state.pending.fetch_and(~bit(signal));
    return true;
}

// --- Delivery -----------------------------------------------------------------------------------------

/// Builds the Linux signal frame on the guest's stack, or its alternate stack, and turns `context`
/// into the handler's entry, as the kernel's setup_rt_frame does.
void setup_frame(ThreadSignals &state, int signal, const LinuxSiginfo &info, const LinuxSigaction &action,
                 GuestContext &context, const Fault *fault) {
    // A thread stopped inside an x18 island goes back to a guest state before anything reads it
    // (ADR-0020 X4): the frame would overwrite the red zone the island saved its registers in.
    if (std::uint64_t x18 = apkrt_get_guest_x18(); islands::leave(context, x18)) apkrt_set_guest_x18(x18);
    const auto alternate = state.alternate_stack;
    const bool alternate_usable = (alternate.flags & ss_disable) == 0 && alternate.size != 0;
    const bool on_alternate =
        alternate_usable && context.sp > alternate.sp && context.sp - alternate.sp <= alternate.size;
    std::uint64_t top = context.sp;
    const bool switch_stack = (action.flags & sa_onstack) != 0 && alternate_usable && !on_alternate;
    if (switch_stack) top = alternate.sp + alternate.size;

    const auto record = (top - 16) & ~std::uint64_t{15};  // struct frame_record: fp, lr
    const auto frame_address = record - sizeof(LinuxSigframe);
    auto *frame = reinterpret_cast<LinuxSigframe *>(frame_address);
    std::memset(frame, 0, sizeof *frame);
    if (action.flags & sa_siginfo) frame->info = info;
    auto &uc = frame->uc;
    uc.stack = alternate;
    uc.sigmask = state.frame_mask_set ? state.frame_mask : state.mask.load();
    state.frame_mask_set = false;
    uc.fault_address = fault != nullptr ? fault->address : 0;
    std::memcpy(uc.regs, context.x, sizeof uc.regs);
    uc.regs[18] = apkrt_get_guest_x18();  // XNU clears the register itself; the guest's x18 is in slot 11
    uc.sp = context.sp;
    uc.pc = context.pc;
    uc.pstate = context.nzcv & (0xf000'0000 | kept_pstate_bits());  // NZCV and the kept bits (ADR-0047 b2)
    auto *fpsimd = reinterpret_cast<FpsimdRecord *>(uc.reserved);
    fpsimd->head = {fpsimd_magic, sizeof(FpsimdRecord)};
    fpsimd->fpsr = static_cast<std::uint32_t>(context.fpsr);
    fpsimd->fpcr = static_cast<std::uint32_t>(context.fpcr);
    std::memcpy(fpsimd->v, context.q, sizeof fpsimd->v);
    std::size_t used = sizeof(FpsimdRecord);
    if (fault != nullptr && fault->esr != 0) {
        auto *esr = reinterpret_cast<EsrRecord *>(uc.reserved + used);
        esr->head = {esr_magic, sizeof(EsrRecord)};
        esr->esr = fault->esr;
        used += sizeof(EsrRecord);
    }
    // The end record {0, 0} is there already: the frame was cleared.
    auto *const link = reinterpret_cast<std::uint64_t *>(record);
    link[0] = context.x[29];
    link[1] = context.x[30];

    context.x[0] = static_cast<std::uint64_t>(signal);
    if (action.flags & sa_siginfo) {
        context.x[1] = frame_address;
        context.x[2] = frame_address + offsetof(LinuxSigframe, uc);
    }
    context.x[29] = record;
    context.x[30] = (action.flags & sa_restorer) ? action.restorer : default_restorer.load();
    context.sp = frame_address;
    context.pc = action.handler;

    std::uint64_t mask = state.mask.load() | action.mask;
    if ((action.flags & sa_nodefer) == 0) mask |= bit(signal);
    state.mask.store(mask & ~unblockable);
    if (switch_stack && (alternate.flags & ss_autodisarm) != 0) state.alternate_stack = {0, ss_disable, 0, 0};
    if (action.flags & sa_resethand) {
        const SpinLock lock(actions_lock);
        actions[signal] = {sig_dfl, action.flags & ~(sa_siginfo | sa_resethand), 0, 0};
    }
}

/// Delivers the thread's deliverable signals on top of `context`: each handler gets its frame,
/// the last one set up runs first, as with the kernel's loop on the way to user space. Signals
/// whose action ignores them are dropped; default actions that end the process do that.
void deliver_pending(ThreadSignals &state, GuestContext &context) {
    for (int round = 0; round < signal_count; ++round) {
        int signal;
        LinuxSiginfo info;
        {
            const SpinLock lock(state.lock);
            if (!take_locked(state, ~state.mask.load(), signal, info)) break;
        }
        // After the program's end nothing is delivered; its SIGKILL comes after the mark (ADR-0028).
        if (process::ended()) process::stop();
        const auto chosen = action(signal);
        if (ignored(chosen, signal)) continue;
        if (chosen.handler == sig_dfl) {
            die_by(signal);
            return;
        }
        setup_frame(state, signal, info, chosen, context, nullptr);
    }
    // Every way back into guest code comes by here: after the program's end the thread stops instead.
    if (process::ended()) process::stop();
    // A wait with a mask of its own (rt_sigsuspend, ppoll, pselect6, epoll_pwait) whose signals got
    // no handler's frame, dropped as ignored: the thread's own mask holds again, as on Linux when no
    // handler runs.
    if (state.frame_mask_set) {
        state.mask.store(state.frame_mask);
        state.frame_mask_set = false;
    }
}

/// Linux's signal, code and address for a fault the hardware raised in guest code.
struct Translated {
    int signal;
    std::int32_t code;
    Fault fault;
};

/// Whether `address` lies in any mapping of the process. Below 4 GiB lies Darwin's PAGEZERO, which a
/// Linux process does not have: nothing is mapped there for the guest.
bool mapped(std::uint64_t address) {
    if (address < (std::uint64_t{1} << 32)) return false;
    vm_address_t region = static_cast<vm_address_t>(address);
    vm_size_t size = 0;
    vm_region_basic_info_data_64_t information;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    const auto result = ::vm_region_64(mach_task_self(), &region, &size, VM_REGION_BASIC_INFO_64,
                                       reinterpret_cast<vm_region_info_t>(&information), &count, &object);
    return result == KERN_SUCCESS && region <= address;
}

/// `pc` is where the guest sees the fault: the site of an x18 island it faulted in.
Translated translate(int host_signal, const siginfo_t &host_info, const ucontext_t &uc, std::uint64_t pc) {
    const auto &exception = uc.uc_mcontext->__es;
    const std::uint64_t esr = exception.__esr;
    switch (host_signal) {
        case SIGSEGV:
        case SIGBUS: {
            constexpr std::uint64_t alignment_fault = 0x21;  // ESR DFSC
            const std::uint64_t address = exception.__far;
            if ((esr & 0x3f) == alignment_fault) return {sigbus, 1 /* BUS_ADRALN */, {address, esr}};
            // Darwin reports a fault in a mapping it has as SIGBUS; Linux: SEGV_ACCERR, else SEGV_MAPERR.
            return {sigsegv, mapped(address) ? 2 : 1, {address, esr}};
        }
        case SIGILL: return {sigill, 1 /* ILL_ILLOPC */, {pc, 0}};
        case SIGTRAP: return {sigtrap, 1 /* TRAP_BRKPT */, {pc, 0}};
        default: {  // SIGFPE, with Darwin's codes in Linux's numbers
            std::int32_t code = 0;
            switch (host_info.si_code) {
                case FPE_INTDIV: code = 1; break;
                case FPE_INTOVF: code = 2; break;
                case FPE_FLTDIV: code = 3; break;
                case FPE_FLTOVF: code = 4; break;
                case FPE_FLTUND: code = 5; break;
                case FPE_FLTRES: code = 6; break;
                case FPE_FLTINV: code = 7; break;
                case FPE_FLTSUB: code = 8; break;
                default: break;
            }
            return {sigfpe, code, {reinterpret_cast<std::uint64_t>(host_info.si_addr), 0}};
        }
    }
}

void handle_fault(int host_signal, siginfo_t *host_info, void *raw) {
    auto &uc = *static_cast<ucontext_t *>(raw);
    threads::GuestThread *thread = threads::current();
    if (thread == nullptr || thread->signals.in_runtime.load()) {
        // Not guest code: the fault is the runtime's own. The handler from before the runtime's
        // (the default one, or a sanitizer's) takes over when the instruction faults again.
        for (std::size_t index = 0; index < std::size(fault_signals); ++index) {
            if (fault_signals[index] == host_signal) ::sigaction(host_signal, &previous_fault_actions[index], nullptr);
        }
        return;
    }
    // A fault of a thread that has yet to stop after the program's end: it stops now (ADR-0028).
    if (process::ended()) process::stop();
    if (host_signal == SIGTRAP) {
        if (const auto trap = islands::jump_table_trap(uc.uc_mcontext->__ss.__pc, apkrt_get_guest_x18())) {
            // The guest's handler would return to the same brk forever (ADR-0020 X2d): stop here.
            char message[islands::message_capacity];
            static_cast<void>(::write(STDERR_FILENO, message, islands::describe(*trap, message, sizeof message)));
            die_by(sigtrap);
            return;
        }
    }
    auto &state = thread->signals;
    auto context = context_from(uc);
    // Inside an x18 island, the guest faults at the site (ADR-0020 X4).
    if (std::uint64_t x18 = apkrt_get_guest_x18(); islands::leave(context, x18)) apkrt_set_guest_x18(x18);
    const auto translated = translate(host_signal, *host_info, uc, context.pc);
    const auto chosen = action(translated.signal);
    // A fault that is blocked, ignored or left to the default ends the process, as on Linux.
    if (chosen.handler == sig_dfl || chosen.handler == sig_ign || (state.mask.load() & bit(translated.signal)) != 0) {
        die_by(translated.signal);
        return;
    }
    LinuxSiginfo info{};
    info.signo = translated.signal;
    info.code = translated.code;
    info.fields.address = translated.fault.address;
    setup_frame(state, translated.signal, info, chosen, context, &translated.fault);
    apply(context, uc);
}

/// For a thread stopped `host_returning`: the guest's state after the host call, as if the exit had
/// run to its end, without the SA_RESTART rule: a host call is never made again (ADR-0030, A1).
GuestContext after_host_call(const ucontext_t &uc) {
    auto context = context_from(uc);
    context.pc = context.x[30];  // the return address the exit popped
    return context;
}

/// The runtime signal: resume a context the thread asked for, or deliver guest signals that
/// another thread sent while this one ran guest code. Runtime code finishes first: in the syscall
/// entry the exit delivers them, and in the exit the thread counts as returned (ADR-0025).
void handle_runtime_signal(int, siginfo_t *, void *raw) {
    auto &uc = *static_cast<ucontext_t *>(raw);
    const auto *request = static_cast<const GuestContext *>(::pthread_getspecific(request_key));
    threads::GuestThread *thread = threads::current();
    // Every runtime signal sent before this one arrived has arrived with it: they do not queue, and one
    // sent while this handler runs arrives when it returns (ADR-0030, A3).
    if (thread != nullptr) thread->signals.nudges_seen.store(thread->signals.nudged.load());
    GuestContext context;
    if (request != nullptr) {
        context = *request;
        ::pthread_setspecific(request_key, nullptr);
    } else {
        if (thread == nullptr) return;
        auto &state = thread->signals;
        const auto stopped = where_stopped(uc.uc_mcontext->__ss.__pc, state.in_runtime.load());
        if (stopped == Stopped::in_runtime) return;
        if ((state.pending.load() & ~state.mask.load()) == 0) return;
        if (stopped == Stopped::returning) {
            context = after_syscall_exit(uc, state.pending.load() & ~state.mask.load());
        } else if (stopped == Stopped::leaving_loader) {
            context = after_loader_call(uc);
        } else if (stopped == Stopped::host_returning) {
            context = after_host_call(uc);
        } else {
            context = context_from(uc);
        }
    }
    if (thread != nullptr) {
        auto &state = thread->signals;
        deliver_pending(state, context);
        // Back to guest code, the context asked for or a handler, when this handler returns. A sender
        // that still found the thread in host code sent no runtime signal; a look after the store finds
        // what it added (ADR-0030, A3).
        state.in_runtime.store(guest_code);
        if ((state.pending.load() & ~state.mask.load()) != 0) deliver_pending(state, context);
    }
    apply(context, uc);
}

/// Sends the runtime signal to another guest thread, unless it runs host code (ADR-0030, decision 1):
/// counted while this thread looks and sends, so that the thread's way into host code can wait for it
/// (A3, enter_host_code).
void nudge(ThreadSignals &state) {
    state.nudging.fetch_add(1);
    if (state.in_runtime.load() != host_code) {
        ::pthread_kill(state.host, runtime_signal);
        state.nudged.fetch_add(1);
    }
    state.nudging.fetch_sub(1);
}

void install_handlers() {
    ::pthread_key_create(&request_key, nullptr);
    struct sigaction action {};
    action.sa_sigaction = handle_runtime_signal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigfillset(&action.sa_mask);
    ::sigaction(runtime_signal, &action, nullptr);
    action.sa_sigaction = handle_fault;
    for (std::size_t index = 0; index < std::size(fault_signals); ++index) {
        ::sigaction(fault_signals[index], &action, &previous_fault_actions[index]);
    }
    // A write to a closed pipe: the syscall layer answers EPIPE and raises the guest's SIGPIPE.
    struct sigaction ignore {};
    ignore.sa_handler = SIG_IGN;
    ::sigaction(SIGPIPE, &ignore, nullptr);
}

/// Linux restarts these after a handler only when they return EINTR on their own, never through
/// SA_RESTART: sleeps, waits with a timeout, sigsuspend, sigtimedwait (kernel/signal.c, the
/// ERESTARTNOHAND and ERESTART_RESTARTBLOCK cases); and a socket's calls while it has a timeout for
/// their direction (sock_intr_errno, sockets.h).
bool restartable(std::uint64_t number, const apkrt_syscall_frame &frame) {
    switch (number) {
        case 22:   // epoll_pwait
        case 72:   // pselect6
        case 73:   // ppoll
        case 101:  // nanosleep
        case 115:  // clock_nanosleep
        case 133:  // rt_sigsuspend
        case 137:  // rt_sigtimedwait
        case 441:  // epoll_pwait2
            return false;
        case 98:  // futex: FUTEX_WAIT with a timeout comes back through restart_syscall, as EINTR here
            return frame.x[3] == 0;
        default:
            return sockets::restarts(number, static_cast<int>(frame.x[31]));
    }
}

/// The guest's state after the syscall of `frame` (sp just above the frame), with the SA_RESTART rule
/// for the `deliverable` signals.
GuestContext after_syscall(const apkrt_syscall_frame &frame, std::uint64_t sp, std::uint64_t deliverable) {
    auto context = context_after_syscall(frame, sp);
    constexpr std::uint64_t eintr = static_cast<std::uint64_t>(-4);
    if (frame.x[0] == eintr && restartable(frame.x[8], frame) && (action(next_signal(deliverable)).flags & sa_restart)) {
        // As the kernel's -ERESTARTSYS: back to the svc, with its first argument, after the handler.
        // Without a veneer, the word before the return address is the call of the entry itself.
        context.x[0] = frame.x[31];
        context.pc -= 4;
    }
    return context;
}

}  // namespace

ThreadSignals &current() {
    threads::GuestThread *thread = threads::current();
    return thread != nullptr ? thread->signals : host_thread_signals;
}

void install() {
    std::call_once(installed, install_handlers);
}

void set_default_restorer(std::uint64_t address) {
    default_restorer.store(address);
}

void resume(const GuestContext &context) {
    install();
    sigset_t runtime;
    sigemptyset(&runtime);
    sigaddset(&runtime, runtime_signal);
    ::pthread_sigmask(SIG_UNBLOCK, &runtime, nullptr);
    ::pthread_setspecific(request_key, &context);
    ::pthread_kill(::pthread_self(), runtime_signal);
    // The handler did not run: XNU drops a signal that a thread sends itself once another thread's exit is under
    // way, and holds the thread a moment later. After the program's end the thread stops, as the handler would
    // have stopped it; before it, the thread sends the signal again until it arrives or the exit holds it. Only a
    // signal that does not arrive within a second is the runtime's failure.
    for (int attempt = 0; attempt < 1000; ++attempt) {
        if (process::ended()) process::stop();
        ::usleep(1000);
        ::pthread_kill(::pthread_self(), runtime_signal);
    }
    static const char message[] = "apkrt: a thread could not resume guest code\n";
    static_cast<void>(::write(STDERR_FILENO, message, sizeof message - 1));
    process::exit(127);
}

void enter_guest(std::uint64_t entry, std::uint64_t stack_pointer) {
    GuestContext context{};  // every register 0, the flags, FPSR and FPCR as well
    context.pc = entry;
    context.sp = stack_pointer;
    context.nzcv = live_pstate_bits();  // SSBS and DIT as the thread has them
    resume(context);
}

LinuxSigaction action(int signal) {
    const SpinLock lock(actions_lock);
    return actions[signal];
}

void set_action(int signal, const LinuxSigaction &value) {
    {
        const SpinLock lock(actions_lock);
        actions[signal] = value;
    }
    // POSIX: a signal set to be ignored is dropped where it is pending.
    if (!ignored(value, signal)) return;
    const auto drop = [&](ThreadSignals &state) {
        const SpinLock lock(state.lock);
        if (is_real_time(signal)) {
            int kept = 0;
            for (int index = 0; index < state.queued; ++index) {
                if (state.queue[index].signal != signal) state.queue[kept++] = state.queue[index];
            }
            state.queued = kept;
        }
        state.pending.fetch_and(~bit(signal));
    };
    threads::for_each([&](threads::GuestThread &thread) { drop(thread.signals); });
    drop(process_signals);
}

ThreadSignals::~ThreadSignals() {
    if (const int descriptor = wait_queue.load(); descriptor >= 0) ::close(descriptor);
}

void raise_signal_word(ThreadSignals &state) {
    state.wake.fetch_add(1);
    ::os_sync_wake_by_address_all(&state.wake, sizeof(std::uint32_t), OS_SYNC_WAKE_BY_ADDRESS_NONE);
}

void notify(ThreadSignals &state) {
    raise_signal_word(state);
    // A thread whose kqueue is not there yet makes it before it checks for signals (wait_queue_of).
    if (const int descriptor = state.wait_queue.load(); descriptor >= 0) {
        struct kevent trigger;
        EV_SET(&trigger, 0, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
        ::kevent(descriptor, &trigger, 1, nullptr, 0, nullptr);
    }
}

int wait_queue_of(ThreadSignals &state) {
    if (const int present = state.wait_queue.load(); present >= 0) return present;
    const int made = ::kqueue();
    if (made < 0) return -1;
    // The user event stays triggered until a kevent takes it (EV_CLEAR), whenever notify() came.
    struct kevent user;
    EV_SET(&user, 0, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (::kevent(made, &user, 1, nullptr, 0, nullptr) != 0) {
        ::close(made);
        return -1;
    }
    ::fcntl(made, F_SETFD, FD_CLOEXEC);
    state.wait_queue.store(made);
    return made;
}

long send_to_thread(std::int32_t tid, int signal, const LinuxSiginfo &info) {
    long result = -3;  // ESRCH
    threads::find(tid, [&](threads::GuestThread &thread) {
        result = 0;
        if (signal == 0) return;
        // Linux drops a signal that would be ignored right away, unless it is blocked.
        if (ignored(action(signal), signal) && (thread.signals.mask.load() & bit(signal)) == 0) return;
        result = enqueue(thread.signals, signal, info);
        if (result != 0) return;
        notify(thread.signals);
        // The calling thread takes it at the end of this syscall; another one gets the runtime
        // signal, which also interrupts a blocking call it might be in, unless it runs host code:
        // then the host call's exit finds the signal.
        if (&thread != threads::current() && thread.signals.host != pthread_t{}) nudge(thread.signals);
    });
    return result;
}

void stop_other_threads() {
    const threads::GuestThread *self = threads::current();
    LinuxSiginfo info{};
    info.signo = sigkill;
    threads::for_each([&](threads::GuestThread &thread) {
        if (&thread == self) return;
        enqueue(thread.signals, sigkill, info);
        notify(thread.signals);
        // A thread that has yet to start finds the end on its way into guest code.
        if (thread.signals.host != pthread_t{}) nudge(thread.signals);
    });
}

long send_to_process(int signal, const LinuxSiginfo &info) {
    // The calling thread unless it blocks the signal, else the first thread that does not.
    threads::GuestThread *self = threads::current();
    std::int32_t target = self != nullptr ? self->tid : ::getpid();
    if (signal != 0 && self != nullptr && (self->signals.mask.load() & bit(signal)) != 0) {
        long kept = 1;
        {
            const SpinLock lock(process_signals.lock);
            threads::for_each([&](threads::GuestThread &thread) {
                if (target == self->tid && (thread.signals.mask.load() & bit(signal)) == 0) target = thread.tid;
            });
            // Every thread blocks it: it waits with the process, as with ART, whose signal catcher takes the
            // SIGQUIT every thread blocks in sigwait.
            if (target == self->tid) kept = enqueue_locked(process_signals, signal, info);
        }
        if (kept != 1) {
            if (kept == 0) threads::for_each([](threads::GuestThread &thread) { notify(thread.signals); });
            return kept;
        }
    }
    return send_to_thread(target, signal, info);
}

void claim_process_signals(ThreadSignals &state) {
    if (process_signals.pending.load() == 0) return;
    const SpinLock lock(process_signals.lock);
    int signal;
    LinuxSiginfo info;
    while (take_locked(process_signals, ~state.mask.load(), signal, info)) enqueue(state, signal, info);
}

bool take(ThreadSignals &state, std::uint64_t allowed, int &signal, LinuxSiginfo &info) {
    {
        const SpinLock lock(state.lock);
        if (take_locked(state, allowed, signal, info)) return true;
    }
    const SpinLock lock(process_signals.lock);  // the thread's own first, as Linux's dequeue_signal
    return take_locked(process_signals, allowed, signal, info);
}

std::uint64_t pending(const ThreadSignals &state) {
    return state.pending.load() | process_signals.pending.load();
}

namespace {

bool within(std::uint64_t pc, const void *begin, const void *end) {
    return pc >= reinterpret_cast<std::uint64_t>(begin) && pc < reinterpret_cast<std::uint64_t>(end);
}

}  // namespace

Stopped where_stopped(std::uint64_t pc, bool in_runtime) {
    if (within(pc, apkrt_syscall_returning, apkrt_syscall_returned)) return Stopped::returning;
    if (within(pc, apkrt_loader_back, apkrt_loader_end)) return Stopped::leaving_loader;
    if (within(pc, apkrt_host_call_returning, apkrt_host_call_returned)) return Stopped::host_returning;
    // The entries before their stores, a loader stub on its way into the loader, and apkrt_call_guest's
    // words after the guest code returned, before it sets the flag again.
    if (in_runtime || within(pc, reinterpret_cast<const void *>(&apkrt_syscall_entry), apkrt_syscall_entered) ||
        within(pc, reinterpret_cast<const void *>(&apkrt_host_call), apkrt_host_call_entered) ||
        within(pc, apkrt_loader_stubs, apkrt_loader_back) ||
        within(pc, reinterpret_cast<const void *>(&apkrt_call_guest), apkrt_call_guest_end)) {
        return Stopped::in_runtime;
    }
    return Stopped::in_guest_code;
}

void enter_host_code(ThreadSignals &state) {
    state.in_runtime.store(host_code);
    // Another thread that looked at in_runtime before this store sends its runtime signal: wait for it.
    // One that looks after it finds host code and sends none.
    while (state.nudging.load() != 0) std::this_thread::yield();
    const auto sent = state.nudged.load();
    if (sent == state.nudges_seen.load()) return;  // the handler ran after the last of them
    // The last may still be pending: unblocking the runtime signal delivers what is pending on the
    // thread before pthread_sigmask returns (XNU sets the thread's AST).
    sigset_t runtime;
    sigemptyset(&runtime);
    sigaddset(&runtime, runtime_signal);
    ::pthread_sigmask(SIG_UNBLOCK, &runtime, nullptr);
    state.nudges_seen.store(sent);
}

void leave_host_code(ThreadSignals &state, std::uint8_t side) {
    state.in_runtime.store(side);
    // A sender that still found host code sent no runtime signal: the thread's next look at its pending
    // signals must find what it added (ADR-0030, A3).
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

GuestContext after_syscall_exit(const ucontext_t &uc, std::uint64_t deliverable) {
    const auto &machine = *uc.uc_mcontext;
    if (machine.__ss.__pc != reinterpret_cast<std::uint64_t>(apkrt_syscall_return)) {
        const auto &frame = *reinterpret_cast<const apkrt_syscall_frame *>(machine.__ss.__sp);
        return after_syscall(frame, machine.__ss.__sp + sizeof frame, deliverable);
    }
    // At the ret the frame lies below sp: the registers hold it, the first argument in x16.
    apkrt_syscall_frame frame{};
    for (int index = 0; index < 29; ++index) frame.x[index] = machine.__ss.__x[index];
    frame.x[29] = machine.__ss.__fp;
    frame.x[30] = machine.__ss.__lr;
    frame.x[31] = machine.__ss.__x[16];
    frame.nzcv = machine.__ss.__cpsr & 0xf000'0000u;
    frame.fpsr = machine.__ns.__fpsr;
    std::memcpy(frame.q, machine.__ns.__v, sizeof frame.q);
    return after_syscall(frame, machine.__ss.__sp, deliverable);
}

GuestContext after_loader_call(const ucontext_t &uc) {
    auto context = context_from(uc);
    if (context.pc == reinterpret_cast<std::uint64_t>(apkrt_loader_back)) {  // before the ldp x16, x30, [sp], #16
        const auto *saved = reinterpret_cast<const std::uint64_t *>(context.sp);
        context.x[16] = saved[0];
        context.x[30] = saved[1];
        context.sp += 16;
    }
    context.pc = context.x[30];
    return context;
}

ThreadSignals *at_syscall_exit() {
    threads::GuestThread *thread = threads::current();
    return thread != nullptr ? &thread->signals : nullptr;
}

void deliver_at_syscall_exit(const apkrt_syscall_frame &frame) {
    auto &state = current();
    resume(after_syscall(frame, reinterpret_cast<std::uint64_t>(&frame + 1), state.pending.load() & ~state.mask.load()));
}

bool read_frame(std::uint64_t address, GuestContext &context) {
    if (address % 16 != 0 || address == 0) return false;
    const auto &frame = *reinterpret_cast<const LinuxSigframe *>(address);
    const auto &uc = frame.uc;
    const FpsimdRecord *fpsimd = nullptr;
    for (std::size_t offset = 0; offset + sizeof(RecordHeader) <= sizeof uc.reserved;) {
        const auto *head = reinterpret_cast<const RecordHeader *>(uc.reserved + offset);
        if (head->magic == 0) break;
        if (head->size < sizeof(RecordHeader) || head->size > sizeof uc.reserved - offset) return false;
        if (head->magic == fpsimd_magic && head->size == sizeof(FpsimdRecord)) {
            fpsimd = reinterpret_cast<const FpsimdRecord *>(head);
        }
        offset += head->size;
    }
    if (fpsimd == nullptr) return false;
    std::memcpy(context.x, uc.regs, sizeof context.x);
    apkrt_set_guest_x18(uc.regs[18]);
    context.sp = uc.sp;
    context.pc = uc.pc;
    // NZCV, and the kept bits as the frame has them, as Linux's restore_sigframe takes PSTATE (ADR-0047 b2).
    context.nzcv = uc.pstate & (0xf000'0000 | kept_pstate_bits());
    context.fpsr = fpsimd->fpsr;
    context.fpcr = fpsimd->fpcr;
    std::memcpy(context.q, fpsimd->v, sizeof context.q);
    auto &state = current();
    state.mask.store(uc.sigmask & ~unblockable);
    claim_process_signals(state);
    auto stack = uc.stack;
    stack.flags &= ss_disable | ss_autodisarm;
    state.alternate_stack = stack;
    // Last, once the whole frame is restored: a handler that returns into a relocated exclusive window
    // body must re-enter the island, not the original body, which iOS runs with x18 cleared to 0 (X4h).
    islands::reenter(context, apkrt_get_guest_x18());
    return true;
}

}  // namespace apkrt::signals

// The entry's context has NZCV at most: the kept PSTATE bits stay as the thread has them.
extern "C" void apkrt_call_guest_enter(const apkrt::GuestContext *context) {
    auto entry = *context;
    entry.nzcv |= apkrt::live_pstate_bits();
    apkrt::signals::resume(entry);
}

extern "C" void apkrt_host_call_deliver(const apkrt::GuestContext *context) {
    auto back = *context;
    back.nzcv |= apkrt::live_pstate_bits();
    apkrt::signals::resume(back);
}

extern "C" void apkrt_settle_runtime_signals(apkrt::signals::ThreadSignals *state) {
    apkrt::signals::enter_host_code(*state);
}
