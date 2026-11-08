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
/// starts a 1 Hz dispatch timer and a DISPATCH_SOURCE_TYPE_MEMORYPRESSURE source that call write_now (the timer with
/// warning false, the pressure source with true). -1 turns the writing off (the sources, started once, then write
/// nothing). One game per process, so the sources are never cancelled.
void report_to(int descriptor);

}  // namespace apkrt::memory
