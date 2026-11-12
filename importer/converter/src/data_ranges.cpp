#include "apkconv/data_ranges.h"

#include "apkconv/data_hints.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <map>
#include <optional>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <utility>

// What the analysis can still get wrong, and in which direction. A word is data only if a load in a trusted run proves
// it and no run of any kind reaches it; every contradiction leaves the word as code. So it can miss a rewrite only
// where a proven load names words that are also instructions that nothing reaches (code that no root, branch or call
// names, read by trusted code on purpose or by mistake). Everything else lands on a refusal, a false one where a
// proof exists but the analysis does not follow it: loads behind a call, a system call or a trap, loads in functions
// that only a call, a pointer, an FDE or a label reaches, tables read through an unbounded index or a base from
// memory. A word that is not proven data is scanned as before the split: a pool word that looks like svc #0, a
// thread-pointer read, mrs CTR_EL0 or an x18 use an island handles is rewritten, silently.

namespace apk::data {
namespace {

using Bytes = std::span<const std::byte>;

std::int64_t sign_extend(std::uint64_t value, unsigned bits) {
    const auto shift = 64 - bits;
    return static_cast<std::int64_t>(value << shift) >> shift;
}

/// The code sections as one run of words: a word's index is its place in the sections taken in address order.
class Index {
public:
    explicit Index(std::span<const Code> code) : code_(code.begin(), code.end()) {
        std::ranges::sort(code_, {}, &Code::address);
        std::size_t total = 0;
        for (const auto &section : code_) {
            first_.push_back(total);
            total += section.bytes.size() / 4;
        }
        first_.push_back(total);
        total_ = total;
    }

    std::size_t words() const { return total_; }

    /// True when two sections share an address: a range could then belong to either, so none is ever reported.
    bool overlapping() const {
        for (std::size_t i = 1; i < code_.size(); ++i) {
            if (code_[i].address - code_[i - 1].address < code_[i - 1].bytes.size()) return true;
        }
        return false;
    }

    /// The word at `address`; none for an unaligned address or one outside every section.
    std::optional<std::size_t> word(std::uint64_t address) const {
        auto section = std::ranges::upper_bound(code_, address, {}, &Code::address);
        if (section == code_.begin()) return std::nullopt;
        --section;
        const auto offset = address - section->address;
        if (offset % 4 != 0 || offset / 4 >= section->bytes.size() / 4) return std::nullopt;
        return first_[static_cast<std::size_t>(section - code_.begin())] + offset / 4;
    }

    std::size_t section_of(std::size_t word) const {
        return static_cast<std::size_t>(std::ranges::upper_bound(first_, word) - first_.begin()) - 1;
    }
    std::size_t section_end(std::size_t word) const { return first_[section_of(word) + 1]; }
    std::uint64_t address(std::size_t word) const {
        const auto section = section_of(word);
        return code_[section].address + (word - first_[section]) * 4;
    }
    std::uint32_t at(std::size_t word) const {
        const auto section = section_of(word);
        std::uint32_t value;
        std::memcpy(&value, code_[section].bytes.data() + (word - first_[section]) * 4, 4);
        return value;
    }
    /// The bytes [address, address + size) when they lie inside one section.
    std::optional<Bytes> bytes(std::uint64_t address, std::uint64_t size) const {
        auto section = std::ranges::upper_bound(code_, address, {}, &Code::address);
        if (section == code_.begin()) return std::nullopt;
        --section;
        const auto offset = address - section->address;
        if (offset > section->bytes.size() || size > section->bytes.size() - offset) return std::nullopt;
        return section->bytes.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(size));
    }
    /// The section's end rounded down to a whole word, for a range that runs to it.
    const std::vector<Code> &sections() const { return code_; }

private:
    std::vector<Code> code_;
    std::vector<std::size_t> first_;
    std::size_t total_ = 0;
};

struct Candidate {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    bool table = false;
};

struct Reg {
    bool known = false;
    bool page = false;  // adrp alone: only the page is known
    bool used = false;  // a load or a store read through it, or an add moved it on
    std::uint64_t value = 0;
};

/// One decoded instruction that a jump table's or a bound's pattern is made of, kept for the last words of a run.
struct Event {
    enum class Kind { index_load, add, compare, condition, write } kind = Kind::condition;
    unsigned dst = 0;                    // index_load: the value register; add: the sum register; write: the register
    unsigned a = 0;                      // index_load: index register; add: first operand; compare: the register
    unsigned b = 0;                      // add: second operand
    std::optional<std::uint64_t> base;   // index_load: the table's address; add: the first operand's value
    std::optional<std::uint64_t> other;  // add: the second operand's value
    unsigned size = 0;                   // index_load: bytes of an entry
    bool is_signed = false;
    bool scaled = false;
    unsigned shift = 0;                  // add: the left shift of the second operand
    std::uint64_t immediate = 0;         // compare
    unsigned cond = 0;                   // condition: the branch's condition code
};

/// The calls that never return, by the name of the import: after one of them the next words are no code.
bool no_return_name(std::string_view name) {
    static constexpr std::string_view names[] = {
        "abort",           "__stack_chk_fail", "__cxa_throw",    "__cxa_rethrow",     "_Unwind_Resume",
        "exit",            "_exit",            "__assert",       "__assert2",         "_ZSt9terminatev",
        "__cxa_pure_virtual", "__libc_fatal",  "__cxa_bad_cast", "__cxa_bad_typeid",  "__fortify_fatal"};
    return std::ranges::find(names, name) != std::end(names);
}

class Analysis {
public:
    Analysis(const Index &index, const Roots &roots)
        : index_(index), reached_(index.words(), 0), no_return_(roots.no_return.begin(), roots.no_return.end()),
          functions_(roots.functions) {
        // Only a trusted root starts a run that proves. A root without proof (a label, an FDE start, the entry
        // point) reaches, so that its words stay code, but what it runs never loads.
        for (const auto address : roots.strong) root(address, true);
        for (const auto address : roots.probable) root(address, false);
        for (const auto address : roots.weak) root(address, false);
        all_roots_.insert(all_roots_.end(), roots.strong.begin(), roots.strong.end());
        all_roots_.insert(all_roots_.end(), roots.probable.begin(), roots.probable.end());
        // Pointers from data end a dispatch's span too (stripped libraries have no better function starts), but only
        // the ones that can be an instruction: aligned and inside a section; a stray 8-byte value is not one.
        for (const auto address : roots.weak) {
            if (index_.word(address)) all_roots_.push_back(address);
        }
        std::ranges::sort(all_roots_);
        // By start, then end: no order of the input shows.
        std::ranges::sort(functions_, [](const Range &a, const Range &b) {
            return std::tie(a.begin, a.end) < std::tie(b.begin, b.end);
        });
    }

    bool gave_up() const { return gave_up_; }

    void run() {
        // Trusted code first: only it loads, and only up to its first barrier. What it only takes the address of, what
        // only data points at and everything behind a barrier is weak: it blocks, it never loads.
        strong_ = true;
        drain(pending_strong_);
        strong_ = false;
        drain(pending_weak_);
    }

    DataRanges finish() {
        DataRanges result;
        result.functions = functions_;
        for (const auto level : reached_) result.reached_words += level != 0 ? 1 : 0;
        result.unbounded_tables = unbounded_;
        result.unproven_bases = unproven_bases_;
        std::ranges::sort(result.unproven_bases);
        // The reach of every word, in runs.
        for (std::size_t word = 0; word < reached_.size();) {
            auto last = word;
            while (last + 1 < reached_.size() && reached_[last + 1] == reached_[word] &&
                   index_.section_of(last + 1) == index_.section_of(word)) {
                ++last;
            }
            result.reach.push_back({index_.address(word), index_.address(last) + 4, reached_[word]});
            word = last + 1;
        }
        // What an unresolved register branch may reach: its function, or the stretch between two roots.
        std::vector<Range> poisoned;
        for (const auto address : dispatches_) poisoned.push_back(dispatch_span(address));
        poisoned = merged(std::move(poisoned));
        std::ranges::sort(candidates_, [](const Candidate &a, const Candidate &b) {
            return std::pair(a.begin, a.end) < std::pair(b.begin, b.end);
        });
        candidates_.erase(std::unique(candidates_.begin(), candidates_.end(),
                                      [](const Candidate &a, const Candidate &b) {
                                          return a.begin == b.begin && a.end == b.end && a.table == b.table;
                                      }),
                          candidates_.end());
        std::ranges::sort(unresolved_);
        // How many words before each word are reached, so that a candidate asks in one step.
        std::vector<std::uint32_t> reached_before(reached_.size() + 1, 0);
        for (std::size_t word = 0; word < reached_.size(); ++word) {
            reached_before[word + 1] = reached_before[word] + (reached_[word] != 0 ? 1 : 0);
        }
        std::uint64_t cursor = 0;
        for (const auto &candidate : candidates_) {
            // Whole words: a byte that code reads as data makes its word no instruction.
            const auto begin = candidate.begin & ~std::uint64_t{3};
            const auto end = (candidate.end + 3) & ~std::uint64_t{3};
            const auto first = index_.word(begin);
            const auto last = index_.word(end - 4);
            if (!first || !last || index_.section_of(*first) != index_.section_of(*last)) continue;
            if (reached_before[*last + 1] != reached_before[*first]) {  // some word of it is reached
                ++result.dropped_reached;
                continue;
            }
            const auto page = std::ranges::lower_bound(unresolved_, (begin & ~std::uint64_t{0xfff}));
            if (page != unresolved_.end() && *page < end) {
                ++result.dropped_unresolved;
                continue;
            }
            if (overlaps_any(poisoned, begin, end)) {
                ++result.dropped_dispatch;
                continue;
            }
            const auto from = std::max(begin, cursor);
            if (end <= from) continue;
            const auto words = static_cast<std::size_t>((end - from) / 4);
            (candidate.table ? result.table_words : result.literal_words) += words;
            // Touching ranges merge only inside one section: a range never spans two (the header says so).
            const auto last_word = result.ranges.empty() ? std::nullopt : index_.word(result.ranges.back().end - 4);
            const auto first_word = index_.word(from);
            if (!result.ranges.empty() && result.ranges.back().end == from && last_word && first_word &&
                index_.section_of(*last_word) == index_.section_of(*first_word)) {
                result.ranges.back().end = end;
            } else {
                result.ranges.push_back({from, end});
            }
            cursor = end;
        }
        return result;
    }

private:
    void root(std::uint64_t address, bool strong) {
        const auto word = index_.word(address);
        if (!word || reached_[*word] >= (strong ? 2 : 1)) return;  // already reached as strongly: nothing to run again
        (strong ? pending_strong_ : pending_weak_).push_back(*word);
    }

    void drain(std::vector<std::size_t> &pending) {
        while (!pending.empty()) {
            const auto start = pending.back();
            pending.pop_back();
            straight(start);
        }
    }

    static std::vector<Range> merged(std::vector<Range> spans) {
        std::ranges::sort(spans, {}, &Range::begin);
        std::vector<Range> out;
        for (const auto &span : spans) {
            if (!out.empty() && span.begin <= out.back().end) {
                out.back().end = std::max(out.back().end, span.end);
            } else {
                out.push_back(span);
            }
        }
        return out;
    }

    /// Whether [begin, end) touches one of the sorted, disjoint spans.
    static bool overlaps_any(const std::vector<Range> &spans, std::uint64_t begin, std::uint64_t end) {
        const auto next = std::ranges::upper_bound(spans, begin, {}, &Range::begin);
        if (next != spans.end() && next->begin < end) return true;
        return next != spans.begin() && std::prev(next)->end > begin;
    }

    // ---- The tracker: what adr, adrp and add made of the registers, along one straight run.
    void forget(unsigned reg) {
        if (regs_[reg].known && !regs_[reg].used) escape(reg);
        regs_[reg] = {};
        if (reg < 31) event(Event{.kind = Event::Kind::write, .dst = reg});
    }
    void forget_all() {
        for (unsigned reg = 0; reg < 31; ++reg) forget(reg);
        events_.clear();
    }
    /// The value leaves the registers: an address the code takes may be where code starts (weak), a page may hold one.
    void escape(unsigned reg) {
        auto &r = regs_[reg];
        if (!r.known || r.used) return;
        if (r.page) {
            unresolved_.push_back(r.value);
        } else {
            run_roots_.push_back({r.value, false});
        }
        r.used = true;
    }
    void set(unsigned reg, std::uint64_t value, bool page) {
        forget(reg);
        if (reg < 31) regs_[reg] = Reg{.known = true, .page = page, .used = false, .value = value};
    }
    void clobber(unsigned reg) {
        if (reg < 31) forget(reg);
    }
    std::optional<std::uint64_t> value(unsigned reg) const {
        return reg < 31 && regs_[reg].known && !regs_[reg].page ? std::optional(regs_[reg].value) : std::nullopt;
    }
    void use(unsigned reg) {
        if (reg < 31) regs_[reg].used = true;
    }
    void event(Event e) {
        events_.push_back(std::move(e));
        if (events_.size() > 48) events_.pop_front();
    }
    void literal(std::uint64_t begin, std::uint64_t size) { candidates_.push_back({begin, begin + size, false}); }

    // ---- The descent.
    struct PendingRoot {
        std::uint64_t address;
        bool strong;
    };

    /// One straight run. It proves (its loads count, its branch targets are trusted) only as long as `run_strong_`:
    /// from a trusted root up to the first barrier, a call, a system call, a trap, a register branch or a word that
    /// is no instruction a compiler writes. From there on the run only reaches: its words stay code, nothing it loads
    /// is data, what it branches to is weak. Nothing is ever taken back: a word that something reached is code.
    void straight(std::size_t word) {
        regs_ = {};
        events_.clear();
        run_roots_.clear();
        run_strong_ = strong_;
        const auto end = index_.section_end(word);
        // A trusted run ends where the function it starts in ends (a symbol's size, an unwind entry): what follows is
        // not reached by falling through, a literal pool for one.
        std::uint64_t limit = ~std::uint64_t{0};
        if (run_strong_) {
            const auto start = index_.address(word);
            auto next = std::ranges::upper_bound(functions_, start, {}, &Range::begin);
            for (int looked = 0; next != functions_.begin() && looked < 16; ++looked) {
                --next;
                if (start < next->end) {
                    limit = next->end;
                    break;
                }
            }
        }
        for (; word < end; ++word) {
            const std::uint64_t pc = index_.address(word);
            if (run_strong_ && pc >= limit) break;
            const char level = run_strong_ ? 2 : 1;
            if (reached_[word] >= level) break;
            reached_[word] = level;
            const auto w = index_.at(word);
            if (run_strong_) {
                if (((w >> 25) & 0xf) == 0) {  // udf or another reserved encoding: no compiler's code
                    reached_[word] = 1;
                    forget_all();
                    run_strong_ = false;
                    continue;
                }
                if ((w & 0xffe0'001f) == 0xd400'0001 || (w & 0xffe0'001f) == 0xd420'0000) {  // svc, brk
                    forget_all();
                    run_strong_ = false;
                    continue;
                }
            }
            if (!decode(w, pc, word)) break;
        }
        forget_all();
        for (const auto &r : run_roots_) root(r.address, r.strong);
    }

    void branch_to(std::uint64_t target) { run_roots_.push_back({target, run_strong_}); }

    /// One instruction; false when the straight run ends here.
    bool decode(std::uint32_t w, std::uint64_t pc, std::size_t word) {
        const unsigned rd = w & 31, rn = (w >> 5) & 31, rt2 = (w >> 10) & 31, rs = (w >> 16) & 31;
        if ((w & 0x7c00'0000) == 0x1400'0000) {  // b, bl
            const auto target = pc + static_cast<std::uint64_t>(sign_extend(w & 0x03ff'ffff, 26) * 4);
            if ((w & 0x8000'0000) != 0) {  // bl: the callee clobbers; unless it never returns, the call returns
                run_roots_.push_back({target, false});  // a callee is reached, not proven by its caller
                forget_all();
                if (no_return_.contains(target)) return false;
                run_strong_ = false;  // what a call returns to is not known to run
                return true;
            }
            branch_to(target);
            return false;
        }
        if ((w & 0xfe00'0000) == 0xd600'0000) {  // br, blr, ret, eret, authenticated forms
            const auto opc = (w >> 21) & 0xf;
            // br and braa/brab, also after a call: the cases of an unresolved one may be data
            if ((opc == 0 || opc == 8) && !jump_table(rn)) dispatches_.push_back(pc);
            if (opc == 1 || opc == 9) {  // blr, blraa...: a call
                forget_all();
                run_strong_ = false;
                return true;
            }
            return false;
        }
        if ((w & 0xff00'0010) == 0x5400'0000) {  // b.cond
            branch_to(pc + static_cast<std::uint64_t>(sign_extend((w >> 5) & 0x7ffff, 19) * 4));
            Event e;
            e.kind = Event::Kind::condition;
            e.cond = w & 15;
            event(e);
            return true;
        }
        if ((w & 0x7e00'0000) == 0x3400'0000) {  // cbz, cbnz
            branch_to(pc + static_cast<std::uint64_t>(sign_extend((w >> 5) & 0x7ffff, 19) * 4));
            return true;
        }
        if ((w & 0x7e00'0000) == 0x3600'0000) {  // tbz, tbnz
            branch_to(pc + static_cast<std::uint64_t>(sign_extend((w >> 5) & 0x3fff, 14) * 4));
            return true;
        }
        if (!run_strong_) return true;  // weak code only blocks: it never loads, so nothing below is followed
        (void)word;
        if ((w & 0x3b00'0000) == 0x1800'0000) {  // ldr (literal), ldrsw (literal), prfm (literal)
            const auto opc = w >> 30;
            const bool vector = (w & 0x0400'0000) != 0;
            if (!(opc == 3 && !vector)) {
                const std::uint64_t size = vector ? (opc == 0 ? 4 : opc == 1 ? 8 : 16) : (opc == 1 ? 8 : 4);
                literal(pc + static_cast<std::uint64_t>(sign_extend((w >> 5) & 0x7ffff, 19) * 4), size);
            }
            clobber(rd);
            return true;
        }
        if ((w & 0x9f00'0000) == 0x1000'0000 || (w & 0x9f00'0000) == 0x9000'0000) {  // adr, adrp
            const auto imm = (((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3);
            const bool page = (w & 0x8000'0000) != 0;
            const auto delta = static_cast<std::uint64_t>(sign_extend(imm, 21)) * (page ? 4096 : 1);
            set(rd, (page ? pc & ~std::uint64_t{0xfff} : pc) + delta, page);
            return true;
        }
        if ((w & 0xff80'0000) == 0x9100'0000) {  // add xd, xn, #imm{, lsl #12}
            const auto base = value(rn);
            const bool page_base = rn < 31 && regs_[rn].known && regs_[rn].page;
            if ((base || page_base) && rn < 31) {
                const auto imm = static_cast<std::uint64_t>((w >> 10) & 0xfff) << (((w >> 22) & 1) ? 12 : 0);
                const auto sum = regs_[rn].value + imm;
                // The sum replaces the base, or it resolves a page (the add of :lo12:, into any register); a copy of a
                // whole address leaves the original there to be taken.
                if (rd == rn || page_base) use(rn);
                set(rd, sum, false);
            } else {
                clobber(rd);
            }
            return true;
        }
        if ((w & 0xffe0'0000) == 0x8b00'0000 && ((w >> 22) & 3) == 0) {  // add xd, xn, xm{, lsl #n}
            Event e;
            e.kind = Event::Kind::add;
            e.dst = rd;
            e.a = rn;
            e.b = rs;
            e.base = value(rn);
            e.other = value(rs);
            e.shift = (w >> 10) & 0x3f;
            event(e);
            clobber(rd);
            return true;
        }
        if ((w & 0x7f80'001f) == 0x7100'001f) {  // cmp wn/xn, #imm (subs wzr/xzr)
            Event e;
            e.kind = Event::Kind::compare;
            e.a = rn;
            e.immediate = static_cast<std::uint64_t>((w >> 10) & 0xfff) << (((w >> 22) & 1) ? 12 : 0);
            event(e);
            return true;
        }
        if (sets_flags(w)) {  // another flag-setting word: the last compare no longer speaks for the flags
            Event e;
            e.kind = Event::Kind::compare;
            e.a = 0xff;
            event(e);
        }
        if ((w & 0x0a00'0000) == 0x0800'0000) {  // loads and stores
            memory(w, rd, rn, rt2, rs);
            return true;
        }
        clobber(rd);  // any other word: whatever it writes is Rd or nothing the scan follows
        return true;
    }

    /// Flag-setting words other than `cmp #imm`: adds/subs (immediate, shifted and extended register), ands/bics,
    /// ccmp/ccmn.
    static bool sets_flags(std::uint32_t w) {
        if ((w & 0x1f00'0000) == 0x1100'0000 || (w & 0x1f00'0000) == 0x0b00'0000) return (w & 0x2000'0000) != 0;
        if ((w & 0x1f80'0000) == 0x1200'0000 || (w & 0x1f00'0000) == 0x0a00'0000) return ((w >> 29) & 3) == 3;
        if ((w & 0x1fe0'0400) == 0x1a40'0000) return true;   // ccmp, ccmn: register and immediate forms
        if ((w & 0x1fe0'fc00) == 0x1a00'0000) return (w & 0x2000'0000) != 0;  // adcs, sbcs
        return (w & 0xff20'fc07) == 0x1e20'2000 || (w & 0xff20'0c00) == 0x1e20'0400;  // fcmp, fcmpe, fccmp, fccmpe
    }

    void memory(std::uint32_t w, unsigned rd, unsigned rn, unsigned rt2, unsigned rs) {
        const auto size_field = w >> 30;
        const bool vector = (w & 0x0400'0000) != 0;
        const auto opc = (w >> 22) & 3;
        const bool unsigned_offset = (w & 0x3b00'0000) == 0x3900'0000;
        const bool register_offset = (w & 0x3b20'0c00) == 0x3820'0800;
        const bool immediate_form = (w & 0x3b20'0000) == 0x3800'0000;  // unscaled, post-index, pre-index
        const bool atomic = (w & 0x3b20'0c00) == 0x3820'0000;
        const bool pair = (w & 0x3a00'0000) == 0x2800'0000;
        const bool exclusive = (w & 0x3f00'0000) == 0x0800'0000;
        const bool structure = (w & 0xbf00'0000) == 0x0c00'0000 || (w & 0xbf00'0000) == 0x0d00'0000;
        const auto pair_index = (w >> 23) & 3;  // 0 no-allocate, 1 post, 2 offset, 3 pre
        const auto form = (w >> 10) & 3;        // of an immediate form: 0 unscaled, 1 post, 3 pre
        bool load = false;
        if (unsigned_offset || register_offset || immediate_form) {
            load = vector ? (opc & 1) != 0 : (opc != 0 && !(size_field == 3 && opc == 2));
        } else if (pair || exclusive || structure) {
            load = (w & 0x0040'0000) != 0;
        }
        const auto base = rn != 31 ? value(rn) : std::nullopt;
        if (base && (load || atomic)) {
            // Exact evidence only: the bytes whose address the code formed.
            if (load && unsigned_offset) {
                const unsigned shift = vector && opc >= 2 ? 4 : size_field;
                literal(*base + (static_cast<std::uint64_t>((w >> 10) & 0xfff) << shift), std::uint64_t{1} << shift);
            } else if (load && immediate_form) {
                const unsigned shift = vector && opc >= 2 ? 4 : size_field;
                const auto offset = static_cast<std::uint64_t>(sign_extend((w >> 12) & 0x1ff, 9));
                literal(form == 1 ? *base : *base + offset, std::uint64_t{1} << shift);
            } else if (load && pair) {
                const unsigned shift = vector ? 2 + size_field : (size_field == 2 ? 3 : 2);
                const auto offset = static_cast<std::uint64_t>(sign_extend((w >> 15) & 0x7f, 7) * (1 << shift));
                literal(pair_index == 1 ? *base : *base + offset, std::uint64_t{2} << shift);
            } else if (load && register_offset && !vector) {
                Event e;
                e.kind = Event::Kind::index_load;
                e.dst = rd;
                e.a = rs;
                e.base = base;
                e.size = 1u << size_field;
                e.is_signed = opc >= 2;
                e.scaled = ((w >> 12) & 1) != 0;
                event(e);
                indexed_table();
            } else {
                unproven_bases_.push_back(*base);  // a base that a strongly reached access used, its extent unknown
            }
        }
        if (rn < 31) use(rn);
        if (!load) {  // a store (or an atomic) reads its source registers: a pointer in one is taken
            if (rd != rn) escape(rd);
            if ((pair || exclusive) && rt2 != rn) escape(rt2);
        }
        clobber(rd);
        if (pair || exclusive) clobber(rt2);
        if (exclusive || atomic) {
            clobber(rs);
            clobber(rs + 1);  // casp writes the register pair Rs, Rs+1
        }
        if (atomic && rs == 31 && ((w >> 12) & 0xf) == 0xd) {  // ld64b writes eight registers from Rt
            for (unsigned i = 1; i < 8; ++i) clobber(rd + i);
        }
        const bool writeback = (pair && (pair_index == 1 || pair_index == 3)) ||
                               (immediate_form && (form == 1 || form == 3)) ||
                               (w & 0xbf80'0000) == 0x0c80'0000 || (w & 0xbf80'0000) == 0x0d80'0000;
        if (writeback && rn < 31) {
            use(rn);
            clobber(rn);
        }
    }

    /// The entries an indexed load is bounded to. The forms that count, and no other: the branch on "higher" (n + 1
    /// entries) or "higher or same" (n entries) comes after a `cmp wI/xI, #n`, with no other flag-setting word between
    /// the compare and the branch, and the index register I is the load's index register and is not written between
    /// the compare and the load.
    std::optional<std::uint64_t> bound_of(std::size_t load_at) const {
        const Event &load = events_[load_at];
        std::size_t condition = load_at;
        while (condition-- > 0) {
            if (events_[condition].kind == Event::Kind::condition) break;
        }
        if (condition >= load_at) return std::nullopt;
        const auto cond = events_[condition].cond;
        if (cond != 8 && cond != 2) return std::nullopt;
        std::size_t compare = condition;
        while (compare-- > 0) {
            if (events_[compare].kind == Event::Kind::compare) break;
        }
        if (compare >= condition || events_[compare].a != load.a) return std::nullopt;
        for (std::size_t i = compare + 1; i < load_at; ++i) {
            if (events_[i].kind == Event::Kind::write && events_[i].dst == load.a) return std::nullopt;
        }
        const auto count = events_[compare].immediate + (cond == 8 ? 1 : 0);
        if (count == 0 || count > 65536) return std::nullopt;
        return count;
    }

    /// An indexed load from a base the code formed: with a proven bound it reads a table of that many entries, the
    /// data; without one nothing is proven.
    void indexed_table() {
        const std::size_t at = events_.size() - 1;
        const Event &load = events_[at];
        if (!load.base) return;
        const auto count = bound_of(at);
        if (!count || (!load.scaled && load.size != 1) || *load.base % load.size != 0) {
            ++unbounded_;
            unproven_bases_.push_back(*load.base);
            return;
        }
        candidates_.push_back({*load.base, *load.base + *count * load.size, true});
    }

    /// A `br xR` that ends a jump table as compilers build it; true when it is proven. The forms: the table of 4-byte
    /// signed entries read by `ldrsw`, each added to the table's own address; or of 1- or 2-byte entries read by
    /// ldrb/ldrh (zero- or sign-extended), each shifted left by 2 and added to a code label formed by adr/adrp+add. The
    /// bound is bound_of's; every entry must be an aligned address inside a section.
    bool jump_table(unsigned reg) {
        const Event *sum = nullptr;
        std::size_t sum_at = 0;
        for (std::size_t i = events_.size(); i-- > 0;) {
            if (events_[i].kind == Event::Kind::add && events_[i].dst == reg) {
                sum = &events_[i];
                sum_at = i;
                break;
            }
        }
        if (sum == nullptr) return false;
        // Anything that writes the branch register after the sum makes the branch not the sum's.
        const auto writes = [&](std::size_t i, unsigned r) {
            const auto kind = events_[i].kind;
            return (kind == Event::Kind::write || kind == Event::Kind::add || kind == Event::Kind::index_load) &&
                   events_[i].dst == r;
        };
        // (The write event right behind the sum or the load is that instruction's own clobber of its destination.)
        const auto own_clobber = [&](std::size_t i, unsigned r) {
            return i < events_.size() && events_[i].kind == Event::Kind::write && events_[i].dst == r;
        };
        for (std::size_t i = sum_at + 1 + (own_clobber(sum_at + 1, reg) ? 1 : 0); i < events_.size(); ++i) {
            if (writes(i, reg)) return false;
        }
        const Event *load = nullptr;
        std::size_t load_at = 0;
        unsigned loaded = 0;
        for (std::size_t i = sum_at; i-- > 0;) {
            if (events_[i].kind == Event::Kind::index_load && (events_[i].dst == sum->a || events_[i].dst == sum->b)) {
                load = &events_[i];
                load_at = i;
                loaded = events_[i].dst;
                break;
            }
        }
        if (load == nullptr || !load->base) return false;
        // And nothing between the load and the sum changes the loaded register.
        for (std::size_t i = load_at + 1 + (own_clobber(load_at + 1, loaded) ? 1 : 0); i < sum_at; ++i) {
            if (writes(i, loaded)) return false;
        }
        const auto count = bound_of(load_at);
        if (!count || (!load->scaled && load->size != 1) || *load->base % load->size != 0) return false;
        const bool base_is_first = sum->b == loaded;
        const auto origin = base_is_first ? sum->base : sum->other;
        if (!origin) return false;
        const unsigned shift = sum->shift;
        // The shift belongs to the second operand: with the entry as the first one the sum is
        // entry + (origin << shift), which is the table's form only without a shift.
        if (!base_is_first && shift != 0) return false;
        if ((shift != 0 && shift != 2) || (load->size == 4 && shift != 0) || (load->size != 4 && shift != 2)) {
            return false;
        }
        const auto table = *load->base;
        const auto size = load->size;
        // Many dispatches may read the same table: it is read once, and its cases are roots once. (Only a trusted run
        // resolves a table: reached code records no indexed loads, so a table never changes strength.)
        const auto key =
            std::make_tuple(table, static_cast<std::uint64_t>(size), *count, load->is_signed, shift, *origin);
        const auto seen = jump_tables_.find(key);
        if (seen != jump_tables_.end()) return seen->second;
        const auto bytes = index_.bytes(table, *count * size);
        if (!bytes) {  // a table outside the code (clang keeps jump tables in .rodata) is not read and costs nothing
            jump_tables_[key] = false;
            return false;
        }
        // A file may read only so many table entries in all: a hostile one with a table per dispatch is given up.
        table_entries_ += *count;
        if (table_entries_ > max_table_entries) {
            gave_up_ = true;
            return false;
        }
        std::vector<std::uint64_t> targets;
        for (std::uint64_t k = 0; k < *count; ++k) {
            std::uint64_t raw = 0;
            std::memcpy(&raw, bytes->data() + k * size, size);
            const std::int64_t entry = load->is_signed ? sign_extend(raw, 8 * size) : static_cast<std::int64_t>(raw);
            const auto target = *origin + static_cast<std::uint64_t>(entry << shift);
            // Every entry an aligned address inside a section, or the table is not proven.
            if (!index_.word(target)) {
                jump_tables_[key] = false;
                return false;
            }
            targets.push_back(target);
        }
        std::ranges::sort(targets);
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        for (const auto target : targets) run_roots_.push_back({target, run_strong_});
        jump_tables_[key] = true;
        return true;
    }

    /// What an unresolved `br` at `address` may reach: the function that holds it (the FDE), or the stretch between
    /// the root before it and the root after it.
    Range dispatch_span(std::uint64_t address) const {
        // The function with the last start at or before the address (a few nested ones are looked through).
        auto next = std::ranges::upper_bound(functions_, address, {}, &Range::begin);
        for (int looked = 0; next != functions_.begin() && looked < 16; ++looked) {
            --next;
            if (address < next->end) return *next;
        }
        const auto after = std::ranges::upper_bound(all_roots_, address);
        const auto begin = after == all_roots_.begin() ? std::uint64_t{0} : *std::prev(after);
        const auto end = after == all_roots_.end() ? ~std::uint64_t{0} : *after;
        return {begin, end};
    }

    const Index &index_;
    std::vector<char> reached_;  // 0 unreached, 1 weakly, 2 strongly
    std::vector<std::size_t> pending_strong_;
    std::vector<std::size_t> pending_weak_;
    std::vector<PendingRoot> run_roots_;
    std::unordered_set<std::uint64_t> no_return_;
    std::vector<Range> functions_;
    std::vector<std::uint64_t> all_roots_;
    std::vector<std::uint64_t> dispatches_;
    std::map<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t, bool, unsigned, std::uint64_t>, bool> jump_tables_;
    static constexpr std::uint64_t max_table_entries = std::uint64_t{1} << 21;
    std::uint64_t table_entries_ = 0;
    bool gave_up_ = false;
    std::vector<std::uint64_t> unproven_bases_;
    std::vector<Candidate> candidates_;
    std::vector<std::uint64_t> unresolved_;
    std::array<Reg, 32> regs_{};
    std::deque<Event> events_;
    std::size_t unbounded_ = 0;
    bool strong_ = true;      // the phase: strong code first, then weak
    bool run_strong_ = true;  // the run in progress
};

}  // namespace

/// The relative and indirect relative relocations of a SHT_ANDROID_RELA table (APS2), as runs: a group that reads
/// nothing per relocation is one run of `count` relocations `delta` apart, every other relocation is a run of one.
std::vector<PackedRun> packed_relocations(Bytes content, bool *complete) {
    std::vector<PackedRun> runs;
    if (complete != nullptr) *complete = true;
    if (content.size() < 4 || std::memcmp(content.data(), "APS2", 4) != 0) return runs;
    std::size_t at = 4;
    bool ok = true;
    const auto sleb = [&]() -> std::int64_t {
        std::uint64_t value = 0;
        unsigned shift = 0;
        while (at < content.size() && shift < 64) {
            const auto byte = static_cast<std::uint8_t>(content[at++]);
            value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
            shift += 7;
            if ((byte & 0x80) == 0) {
                if (shift < 64 && (byte & 0x40) != 0) value |= ~std::uint64_t{0} << shift;
                return static_cast<std::int64_t>(value);
            }
        }
        ok = false;
        return 0;
    };
    auto remaining = static_cast<std::uint64_t>(sleb());
    std::uint64_t offset = static_cast<std::uint64_t>(sleb());  // the offset the first delta is added to
    std::uint64_t addend = 0;
    while (ok && remaining > 0) {
        const auto group_size = static_cast<std::uint64_t>(sleb());
        const auto flags = static_cast<std::uint64_t>(sleb());
        if (!ok || group_size == 0 || group_size > remaining) break;  // a group that cannot be read: cut short
        const bool by_info = (flags & 1) != 0, by_delta = (flags & 2) != 0, by_addend = (flags & 4) != 0,
                   has_addend = (flags & 8) != 0;
        if (!has_addend) addend = 0;  // as bionic's reader: a group without addends has the addend 0
        std::uint64_t group_delta = 0;
        if (by_delta) group_delta = static_cast<std::uint64_t>(sleb());
        std::uint64_t info = 0;
        if (by_info) info = static_cast<std::uint64_t>(sleb());
        if (has_addend && by_addend) addend += static_cast<std::uint64_t>(sleb());
        // A group that reads nothing per relocation (every field is shared) has the counts of the file and no more
        // bytes: all its relocations carry the same info and addend, so one run says what each of them says.
        const bool reads_per_relocation = !by_delta || !by_info || (has_addend && !by_addend);
        if (!reads_per_relocation) {
            offset += group_delta;
            if ((info & 0xffff'ffff) == 1027 || (info & 0xffff'ffff) == 1032) {
                runs.push_back({offset, group_delta, group_size, addend});
            }
            offset += group_delta * (group_size - 1);
        } else {
            for (std::uint64_t k = 0; k < group_size && ok; ++k) {
                offset += by_delta ? group_delta : static_cast<std::uint64_t>(sleb());
                if (!by_info) info = static_cast<std::uint64_t>(sleb());
                if (has_addend && !by_addend) addend += static_cast<std::uint64_t>(sleb());
                if (ok && ((info & 0xffff'ffff) == 1027 || (info & 0xffff'ffff) == 1032)) {
                    runs.push_back({offset, 0, 1, addend});
                }
            }
        }
        remaining -= group_size;
    }
    // Whatever stopped the reading before the promised count is a table that is cut short.
    if ((!ok || remaining != 0) && complete != nullptr) *complete = false;
    return runs;
}

/// The values of a SHT_ANDROID_RELA table (APS2): the addend of each run.
std::vector<std::uint64_t> packed_relocation_addends(Bytes content) {
    std::vector<std::uint64_t> addends;
    for (const auto &run : packed_relocations(content)) addends.push_back(run.addend);
    return addends;
}

/// Whether a relocation of `run` lies in [begin, end): the offsets are offset + k * delta for k below the count.
bool run_touches(const PackedRun &run, std::uint64_t begin, std::uint64_t end) {
    if (run.offset >= end) return false;
    if (run.offset >= begin) return true;
    if (run.delta == 0) return false;
    const std::uint64_t steps = (begin - run.offset + run.delta - 1) / run.delta;  // the first k at or past begin
    return steps < run.count && run.offset + steps * run.delta < end;
}

Reach DataRanges::reach_of(std::uint64_t address) const {
    const auto run = std::ranges::upper_bound(reach, address, {}, &ReachRun::begin);
    if (run == reach.begin()) return Reach::none;
    const auto &found = *std::prev(run);
    return address < found.end ? static_cast<Reach>(found.level) : Reach::none;
}

bool DataRanges::in_different_functions(std::uint64_t a, std::uint64_t b) const {
    // The function with the last start at or before the address (a few nested ones are looked through).
    const auto find = [&](std::uint64_t address) -> const Range * {
        auto next = std::ranges::upper_bound(functions, address, {}, &Range::begin);
        for (int looked = 0; next != functions.begin() && looked < 16; ++looked) {
            --next;
            if (address < next->end) return &*next;
        }
        return nullptr;
    };
    const auto *first = find(a);
    const auto *second = find(b);
    return first != nullptr && second != nullptr && (first->begin != second->begin || first->end != second->end);
}

bool DataRanges::behind_unproven_base(std::uint64_t address) const {
    const auto run = std::ranges::upper_bound(reach, address, {}, &ReachRun::begin);
    if (run == reach.begin()) return false;
    const auto &found = *std::prev(run);
    if (address >= found.end || found.level != 0) return false;
    const auto base = std::ranges::lower_bound(unproven_bases, found.begin);
    return base != unproven_bases.end() && *base <= address;
}

DataRanges find_ranges(std::span<const Code> code, const Roots &roots) {
    const Index index(code);
    if (index.overlapping()) return {};
    Analysis analysis(index, roots);
    analysis.run();
    if (analysis.gave_up()) return {};
    return analysis.finish();
}

namespace {

/// The PLT entries of the imports that never return: an entry is `adrp x16, page; ldr x17, [x16, #off]` (a `bti c`
/// may come first), and the relocation of the GOT slot it loads from names the import.
std::vector<std::uint64_t> no_return_calls(std::span<const elf::Section> sections,
                                           std::span<const std::byte> elf_bytes) {
    std::vector<std::uint64_t> calls;
    const auto bytes_of = [&](const elf::Section &section) -> Bytes {
        if (section.type == elf::section_type_nobits || section.offset > elf_bytes.size() ||
            section.size > elf_bytes.size() - section.offset) {
            return {};
        }
        return elf_bytes.subspan(static_cast<std::size_t>(section.offset), static_cast<std::size_t>(section.size));
    };
    const elf::Section *plt = nullptr, *rela = nullptr, *dynsym = nullptr, *dynstr = nullptr;
    for (const auto &section : sections) {
        if (section.name == ".plt") plt = &section;
        if (section.name == ".rela.plt") rela = &section;
        if (section.type == elf::section_type_dynsym) dynsym = &section;
        if (section.name == ".dynstr") dynstr = &section;
    }
    if (plt == nullptr || rela == nullptr || dynsym == nullptr || dynstr == nullptr) return calls;
    const auto code = bytes_of(*plt), relocations = bytes_of(*rela);
    const auto symbols = bytes_of(*dynsym), names = bytes_of(*dynstr);
    std::map<std::uint64_t, std::uint64_t> entry_of_slot;  // GOT slot address -> the PLT entry's address
    for (std::size_t at = 0; at + 8 <= code.size(); at += 4) {
        std::uint32_t first, second;
        std::memcpy(&first, code.data() + at, 4);
        std::memcpy(&second, code.data() + at + 4, 4);
        if ((first & 0x9f00'001f) != 0x9000'0010 || (second & 0xffc0'03ff) != 0xf940'0211) continue;
        const auto imm = (((first >> 5) & 0x7ffff) << 2) | ((first >> 29) & 3);
        const auto pc = plt->address + at;
        const auto slot = (pc & ~std::uint64_t{0xfff}) + static_cast<std::uint64_t>(sign_extend(imm, 21)) * 4096 +
                          (((second >> 10) & 0xfff) << 3);
        std::uint32_t before = 0;
        if (at >= 4) std::memcpy(&before, code.data() + at - 4, 4);
        entry_of_slot[slot] = before == 0xd503'245f ? pc - 4 : pc;  // bti c
    }
    for (std::size_t at = 0; at + 24 <= relocations.size(); at += 24) {
        std::uint64_t offset, info;
        std::memcpy(&offset, relocations.data() + at, 8);
        std::memcpy(&info, relocations.data() + at + 8, 8);
        if ((info & 0xffff'ffff) != 1026) continue;  // R_AARCH64_JUMP_SLOT
        const auto index = info >> 32;
        if (index * 24 + 24 > symbols.size()) continue;
        std::uint32_t name_offset;
        std::memcpy(&name_offset, symbols.data() + index * 24, 4);
        if (name_offset >= names.size()) continue;
        const auto *text = reinterpret_cast<const char *>(names.data()) + name_offset;
        const std::string_view name(text, strnlen(text, names.size() - name_offset));
        const auto entry = entry_of_slot.find(offset);
        if (no_return_name(name) && entry != entry_of_slot.end()) calls.push_back(entry->second);
    }
    return calls;
}

}  // namespace

DataRanges find(const elf::File &file, std::span<const std::byte> elf_bytes, std::span<const elf::Section> sections,
                std::span<const elf::Symbol> symbols) {
    const auto bytes_of = [&](const elf::Section &section) -> Bytes {
        if (section.type == elf::section_type_nobits || section.offset > elf_bytes.size() ||
            section.size > elf_bytes.size() - section.offset) {
            return {};
        }
        return elf_bytes.subspan(static_cast<std::size_t>(section.offset), static_cast<std::size_t>(section.size));
    };
    const auto executable = [](const elf::Section &section) {
        return (section.flags & elf::section_allocated) != 0 && (section.flags & elf::section_executable) != 0;
    };
    const auto init_array = [](const elf::Section &section) {
        return section.name == ".init_array" || section.name == ".fini_array" || section.name == ".preinit_array";
    };
    // Sections that overlap in the file read the same bytes again, in the index, in the unwind table and in the roots:
    // every section read here is paid for from one budget of twice the file's size (the unwind table is read twice), or
    // the table is hostile and nothing is split (everything stays code, as before the split).
    std::size_t budget = 2 * elf_bytes.size() + 0x10000;
    const auto spend = [&](std::size_t bytes) {
        if (bytes > budget) return false;
        budget -= bytes;
        return true;
    };
    std::vector<Code> code;
    for (const auto &section : sections) {
        if (executable(section) && section.type != elf::section_type_nobits && section.size != 0) {
            const auto content = bytes_of(section);
            if (!spend(content.size())) return {};
            code.push_back({content, section.address});
        }
    }
    Roots roots;
    roots.probable.push_back(file.entry);  // a header field: nothing says the word is code
    for (const auto &entry : elf::read_dynamic(file, elf_bytes)) {
        if (entry.tag == 12 || entry.tag == 13) roots.strong.push_back(entry.value);  // DT_INIT, DT_FINI
    }
    for (const auto &symbol : symbols) {
        const std::string_view name = symbol.name;
        const bool mapping = name.size() >= 2 && name[0] == '$' && (name[1] == 'x' || name[1] == 'd') &&
                             (name.size() == 2 || name[2] == '.');
        // A function symbol with a size is a function. A label (NOTYPE, a mapping symbol, a function without a size)
        // only says where an author put it, and a pool can carry one: it runs as code unless its word is loaded.
        if (mapping) {
            if (name[1] == 'x') roots.probable.push_back(symbol.value);
        } else if ((symbol.type == 2 || symbol.type == 10) && symbol.size != 0) {
            roots.strong.push_back(symbol.value);
            roots.functions.push_back({symbol.value, symbol.value + symbol.size});
        } else if (symbol.type == 0 || symbol.type == 2 || symbol.type == 10) {
            roots.probable.push_back(symbol.value);
        }
    }
    for (const auto &section : sections) {
        if (section.name != ".eh_frame") continue;
        if (!spend(bytes_of(section).size())) return {};
        bool complete = true;
        for (const auto &fde : hints::fde_ranges(bytes_of(section), section.address, &complete)) {
            roots.probable.push_back(fde.first);  // an FDE may open in front of a pool
            roots.functions.push_back({fde.first, fde.second});
        }
        // The FDEs are roots that reach; a table that is cut short has lost some, which can only make data larger.
        if (!complete) return {};
    }
    // The init and fini arrays, found once. A library has a handful; a header table with hundreds of them is hostile.
    std::vector<const elf::Section *> arrays;
    for (const auto &section : sections) {
        if (init_array(section)) arrays.push_back(&section);
    }
    if (arrays.size() > 16) return {};
    const auto in_init_array = [&](std::uint64_t address) {
        return std::ranges::any_of(arrays, [&](const elf::Section *section) {
            return address >= section->address && address - section->address < section->size;
        });
    };
    for (const auto &section : sections) {
        const auto content = bytes_of(section);
        if (section.type == 4 || section.type == 0x6000'0002 ||
            ((section.flags & elf::section_allocated) != 0 && !executable(section) &&
             section.type != elf::section_type_symtab && section.type != elf::section_type_dynsym)) {
            if (!spend(content.size())) return {};
        }
        if (section.type == 4) {  // SHT_RELA: R_AARCH64_RELATIVE and IRELATIVE carry a code pointer as the addend
            for (std::size_t at = 0; at + 24 <= content.size(); at += 24) {
                std::uint64_t offset, info, addend;
                std::memcpy(&offset, content.data() + at, 8);
                std::memcpy(&info, content.data() + at + 8, 8);
                std::memcpy(&addend, content.data() + at + 16, 8);
                if ((info & 0xffff'ffff) != 1027 && (info & 0xffff'ffff) != 1032) continue;
                (in_init_array(offset) ? roots.strong : roots.weak).push_back(addend);
            }
        } else if (section.type == 0x6000'0002) {  // SHT_ANDROID_RELA (0x60000001 is ANDROID_REL: no addends), APS2
            bool whole = true;
            const auto packed = packed_relocations(content, &whole);
            if (!whole) return {};  // relocations lost, and their addends are roots that reach
            for (const auto &run : packed) {
                // A run that fills an init or fini array holds trusted entries, as the plain table's do.
                const bool in_array = std::ranges::any_of(arrays, [&](const elf::Section *array) {
                    return run_touches(run, array->address, array->address + array->size);
                });
                (in_array ? roots.strong : roots.weak).push_back(run.addend);
            }
        } else if (section.type == elf::section_type_symtab || section.type == elf::section_type_dynsym) {
            // A symbol table's st_value words are no pointers the program holds: an object symbol names data.
        } else if ((section.flags & elf::section_allocated) != 0 && !executable(section)) {
            for (std::size_t at = 0; at + 8 <= content.size(); at += 8) {  // a pointer into code, wherever it is
                std::uint64_t value;
                std::memcpy(&value, content.data() + at, 8);
                (init_array(section) ? roots.strong : roots.weak).push_back(value);
            }
        }
    }
    roots.no_return = no_return_calls(sections, elf_bytes);
    return find_ranges(code, roots);
}

}  // namespace apk::data
