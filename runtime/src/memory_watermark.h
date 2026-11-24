// A crash names its cause (ADR-0032 decision 8), the memory half: game mode has the runtime write the process's
// memory footprint again and again to a separate file (apkrt_report_memory_to -> last-session.memory), so that when
// iOS ends the process for using too much memory -- which leaves no exit status, the empty "ended without a status" --
// the app sees on its next start how much memory the process held right before the end and whether a memory-pressure
// warning had come. Only numbers in a fixed-size block, overwritten in place; nothing a guest wrote.
#pragma once

#include <cstddef>
#include <cstdint>

namespace apkrt::memory {

/// One reading of the process's memory, all in bytes but `warning`.
struct Watermark {
    std::uint64_t footprint = 0;  // the physical footprint now (TASK_VM_INFO phys_footprint)
    std::uint64_t available = 0;  // os_proc_available_memory (iOS only; 0 on the Mac)
    std::uint64_t peak = 0;       // the peak physical footprint so far (ledger_phys_footprint_peak)
    bool warning = false;         // a memory-pressure warning has come since the writing began
};

/// The current reading: task_info for the footprint and its peak, os_proc_available_memory for what is left, and
/// `warning` from the process-wide warning flag (write_now sets it). Safe to call from any thread; no allocation.
Watermark sample();

/// The fixed size format() always writes: four "<name> <value>\n" lines with the values space-padded to 20 digits
/// ("footprint "/"available "/"peak "/"warning " + value + "\n" = 31 + 31 + 26 + 10).
inline constexpr std::size_t block_size = 98;

/// Writes `watermark` as the block into out[0..size): the four lines, each value right-justified in a fixed width so
/// the block is always block_size bytes. Returns the length written (block_size when size >= block_size, else 0). No
/// allocation, so it is safe off any thread.
std::size_t format(const Watermark &watermark, char *out, std::size_t size);

/// ORs `warning` into the process-wide warning flag, then samples and writes the block to `descriptor` at offset 0
/// (the fixed slot, so the last block always overwrites the one before and survives an abrupt end). Does nothing for
/// a descriptor below 0. The pwrite and the sampling are all; no allocation.
void write_now(int descriptor, bool warning);

/// apkrt_report_memory_to: keeps `descriptor` as the slot, writes a first block at once, and -- once per process --
/// starts a 1 Hz sampler thread and a DISPATCH_SOURCE_TYPE_MEMORYPRESSURE source that call write_now (the thread with
/// warning false, the pressure source with true) and log_instrument. -1 turns the block writing off (the sampler,
/// started once, then writes no block). One game per process, so the thread and source are never cancelled.
void report_to(int descriptor);

/// One reading for the 1 Hz / memory-warning instrument (ADR-0032 decision 8, the session-log half): the process's
/// footprint split from TASK_VM_INFO, and the guest's mapped bytes by backing. All in bytes.
struct Instrument {
    std::uint64_t footprint = 0;   // phys_footprint: what iOS counts against the app's limit (the jetsam metric)
    std::uint64_t internal = 0;    // anonymous dirty bytes (malloc, the allocator, engine buffers)
    std::uint64_t compressed = 0;  // bytes the compressor holds (anonymous, paged out under pressure)
    std::uint64_t external = 0;    // file-backed resident bytes (not counted in the footprint)
    std::uint64_t graphics = 0;    // ledger_tag_graphics_footprint: GPU memory (IOSurface, Metal)
    std::uint64_t media = 0;       // ledger_tag_media_footprint
    std::uint64_t purgeable = 0;   // ledger_purgeable_nonvolatile: purgeable bytes still counted in the footprint
    std::uint64_t purgeable_compressed = 0;  // ledger_purgeable_novolatile_compressed (the SDK's spelling)
    std::uint64_t other = 0;       // footprint minus the parts above: IOKit/GPU-wired, page tables, alt accounting
    std::uint64_t anon = 0;        // the guest's (VM tag 0) anonymous mapped bytes (Scudo's regions are here)
    std::uint64_t file = 0;        // the guest's (VM tag 0) file-backed mapped bytes
    std::uint64_t gpu = 0;         // IOAccelerator + IOSurface regions' dirty bytes (info; `graphics` stays the ledger)
    std::uint64_t malloc_bytes = 0;  // old-allocator MALLOC_* regions' dirty bytes (info; ~0 where xzone tags its own)
    std::uint64_t hostmalloc = 0;  // tag 99 dirty bytes: the host allocator's regions (macOS 27 xzone); parking lives
                                   // here, measured apart by the park line's packed+kept (info, a subdivision)
    std::uint64_t untagged = 0;    // dirty bytes of every other region (tag 0, the guest's, and any unnamed tag) (info)
    std::uint64_t virtual_size = 0;  // TASK_VM_INFO's virtual_size: the address space the process holds (va=)
    std::uint64_t largest_hole = 0;  // the largest free range between the task's lowest and highest address (vahole=):
                                     // what the next large reservation (a scudo size class, a library) can still get
};

/// The largest free range of the address space between `low` and `high`, from the mapped regions in address order (a
/// VM-region walk's). No allocation, so it can follow the walk in sample_instrument.
class FreeRange {
public:
    FreeRange(std::uint64_t low, std::uint64_t high) : next_(low), high_(high) {}
    /// A mapped region [start, end), after every region that starts below it.
    void region(std::uint64_t start, std::uint64_t end) {
        const std::uint64_t top = start < high_ ? start : high_;
        if (top > next_ && top - next_ > largest_) largest_ = top - next_;
        if (end > next_) next_ = end;
    }
    /// The largest gap so far, the range from the last region to `high` among them.
    std::uint64_t largest() const {
        const std::uint64_t tail = high_ > next_ ? high_ - next_ : 0;
        return tail > largest_ ? tail : largest_;
    }

private:
    std::uint64_t next_;  // where the free range after the regions so far begins
    std::uint64_t high_;
    std::uint64_t largest_ = 0;
};

/// Samples the Instrument now: task_info for the footprint split, a VM-region walk for the guest's mapped bytes. Safe
/// to call from any thread; no allocation.
Instrument sample_instrument();

/// Formats the instrument line into out[0..size): "I/apkrt   : mem foot=.. int=.. comp=.. gfx=.. media=.. purg=..
/// purgc=.. other=.. ext=.. anon=.. file=.. gpu=.. malloc=.. hmalloc=.. untag=.. va=.. vahole=..[ warn]\n", every byte
/// field in MiB (foot = int+comp+gfx+media+purg+purgc+other; gfx is the graphics ledger). gpu/malloc/hmalloc/untag are
/// VM-region dirty sums by tag, reported to attribute `other`, not added to the identity. va and vahole are the
/// address space held and the largest range still free in it (numbers only). " warn" when `warning`. 0 if it does
/// not fit.
std::size_t format_instrument(const Instrument &reading, bool warning, char *out, std::size_t size);

/// Writes the instrument line to the session log (fd 2) with one write(2) -- so the lines survive a jetsam kill -- only
/// when MINATO_LOADER_LOG is set (the figures are machine-dependent and would break the byte-compared fixtures; the app
/// sets the env in game mode). `warning` marks a memory-pressure / low-memory line. No allocation; safe off any thread.
void log_instrument(bool warning);

/// Starts the 1 Hz sampler thread and the memory-pressure source so log_instrument fires every second and at each OS
/// warning, without report_to's block writing. A pre-created thread, not a dispatch timer: under the memory pressure
/// we record, libdispatch may not spawn a worker to run a timer, but a thread already asleep still wakes. Game mode
/// calls it at session start (gated by MINATO_LOADER_LOG); once per process.
void start_instrument();

/// The signature of libGLESv2's ANGLEMetalMemoryLine: writes a one-line, allocation-free ANGLE/Metal memory summary
/// (e.g. "gl tex=5/2 priv=1 ... mtl=4") into line[0..size) and returns its length. Resolved at run time, not linked.
using GraphicsLineFn = int (*)(char *line, std::size_t size);

/// A lookup for ANGLEMetalMemoryLine: the symbol, or nullptr while ANGLE is not loaded yet. graphics_line retries it
/// every call until it finds the symbol, then caches the pointer; the tests inject one with a counter.
using GraphicsLookupFn = GraphicsLineFn (*)();

/// Builds "I/apkrt   : <stats>[ warn]\n" from ANGLEMetalMemoryLine into out[0..size). The symbol is found by dlsym and,
/// failing that (ANGLE is loaded RTLD_LOCAL, which RTLD_DEFAULT cannot see), by the loaded libGLESv2 image; the lookup
/// is retried each call until it succeeds and the pointer is then cached. Returns the length, or 0 when the symbol is
/// absent (Mac fixtures, pre-0015 ANGLE, or not loaded yet) or it would not fit. log_instrument writes it with one
/// write(2) after each mem line, so the graphics view follows every sample.
std::size_t graphics_line(bool warning, char *out, std::size_t size);

/// Test seam (tests only): replaces the dlsym/_dyld lookup with `lookup` and clears the cached pointer; nullptr
/// restores the real lookup (and clears the cache). Lets a test model a symbol that appears only after a first call.
void set_graphics_lookup_for_test(GraphicsLookupFn lookup);

/// Builds "I/apkrt   : angle <stats>[ warn]\n" from libGLESv2's ANGLEMetalParkingLine (ADR-0075, 0017 texture parking:
/// "park n=... parks=... restores=..."), the same way graphics_line builds the gl line and with the same
/// dlsym/_dyld lookup and caching. Returns the length, or 0 when the symbol is absent (a library without 0017, or ANGLE
/// not loaded yet) or it would not fit. log_instrument writes it right after the gl line.
std::size_t parking_line(bool warning, char *out, std::size_t size);

/// Test seam (tests only): the parking_line counterpart of set_graphics_lookup_for_test.
void set_parking_lookup_for_test(GraphicsLookupFn lookup);

}  // namespace apkrt::memory
