// Checks the A64 register-use decoder (ADR-0019) and the register substitution (ADR-0020) in two
// ways:
//   - reference cases from the host assembler (a64_reference.S): the exact registers each
//     instruction reads and writes, from the Arm ARM; and hand-made substitution cases;
//   - with llvm-objdump as the oracle, on hundreds of thousands of instruction words: the decoder
//     must report exactly the general-purpose registers the disassembly names, plus the implicit
//     ones of the few instructions that have them. That catches fields taken for registers that
//     are none (v18, PRFM's operation 18) and registers the decoder misses. Every word that uses
//     x18 is also substituted, and its disassembly must be the original one with x18 renamed.
//
//   a64_test                                      the reference cases
//   a64_test <llvm-objdump> <scratch> [<fixtures>] the comparison with llvm-objdump
#include "apkconv/a64.h"

#include "apkconv/elf.h"
#include "check.h"
#include "elf_builder.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
extern const std::uint32_t a64_cases[];
extern const std::uint32_t a64_cases_end[];
extern const std::uint32_t a64_expect[];
}

namespace {

using apk::a64::RegisterUse;

std::string registers(std::uint32_t mask) {
    std::string text;
    for (unsigned reg = 0; reg < 31; ++reg) {
        if (mask & (1u << reg)) text += std::format("{}x{}", text.empty() ? "" : ",", reg);
    }
    return text.empty() ? "-" : text;
}

TEST(the_reference_cases_read_and_write_what_the_arm_arm_says) {
    const auto count = static_cast<std::size_t>(a64_cases_end - a64_cases);
    CHECK(count > 300);
    for (std::size_t index = 0; index < count; ++index) {
        const RegisterUse expected{a64_expect[2 * index], a64_expect[2 * index + 1]};
        const auto actual = apk::a64::register_use(a64_cases[index]);
        if (actual != expected) {
            const auto detail = std::format("case {} ({:08x}): reads {} writes {}, expected reads {} writes {}", index,
                                            a64_cases[index], registers(actual.reads), registers(actual.writes),
                                            registers(expected.reads), registers(expected.writes));
            apk::testing::fail(std::source_location::current(), "", detail.c_str());
        }
    }
}

/// A fixed sequence, so that a failure can be reproduced.
class Random {
public:
    std::uint32_t operator()() {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return static_cast<std::uint32_t>((state_ * 0x2545'f491'4f6c'dd1dull) >> 32);
    }

private:
    std::uint64_t state_ = 0x9e37'79b9'7f4a'7c15ull;
};

TEST(substitution_renames_register_fields_and_nothing_else) {
    using apk::a64::substitute;
    // add x18, x18, x1: Rd and Rn.
    CHECK(substitute(0x8b01'0252, 18, 9) == 0x8b01'0129u);
    // ldp x17, x18, [x3, #0x50]: Rt2.
    CHECK(substitute(0xa945'4871, 18, 9) == 0xa945'2471u);
    // ldr x18, [x0, #0x90]: the scaled offset 18 lies where Rt2 would be, and stays.
    CHECK(substitute(0xf940'4812, 18, 9) == 0xf940'4809u);
    // fmov x18, d18: d18 is no general-purpose register.
    CHECK(substitute(0x9e66'0252, 18, 9) == 0x9e66'0249u);
    // cbz x18, #+8 and msr TPIDR_EL0, x18: only Rt; the offset and the system register stay.
    CHECK(substitute(0xb400'0052, 18, 9) == 0xb400'0049u);
    CHECK(substitute(0xd51b'd052, 18, 9) == 0xd51b'd049u);
    // The unallocated part of the 128-bit system space (bits 31:22 = 1101010101): L set with op0 = 0x, or L clear with
    // op0 = 00. The CPU raises SIGILL on such a word, so it is data or dead bytes, and uses no register (a pair
    // instruction there would make a "use" of x18 that nothing can run).
    CHECK(apk::a64::register_use(0xd566'6e72) == RegisterUse{});
    for (std::uint32_t low = 0; low < (1u << 22); ++low) {
        const std::uint32_t word = 0xd540'0000u | low;
        if ((word >> 22) != 0b1101010101) continue;
        const bool l = (word >> 21 & 1) != 0;
        const unsigned op0 = word >> 19 & 3;
        if ((l && op0 < 2) || (!l && op0 == 0)) {
            if (apk::a64::register_use(word) != RegisterUse{}) {
                std::printf("unallocated 128-bit system word %08x uses registers\n", word);
                CHECK(false);
                break;
            }
        }
    }
    // A word without x18 stays as it is.
    CHECK(substitute(0x8b02'0020, 18, 9) == 0x8b02'0020u);
    // Refused: x18 without a field of its own (st64b x16 uses x16 to x23), a register pair (casp),
    // a replacement the instruction uses already, and sp/zr.
    CHECK(!substitute(0xf83f'9010, 18, 9));
    CHECK(!substitute(0x4832'7c02, 18, 9));
    CHECK(!substitute(0x8b01'0252, 18, 1));
    CHECK(!substitute(0x8b01'0252, 18, 31));
}

// The groups with op1 (bits 28:25) = 0000 to 0011: reserved (UDF), SME, SVE and unallocated.
// None of it can run, so none of it uses a register, whatever its fields say.
TEST(sve_sme_and_the_reserved_groups_use_no_registers) {
    Random random;
    for (int index = 0; index < 100'000; ++index) {
        const auto word = (random() & ~(0xfu << 25)) | ((random() % 4) << 25);
        const auto use = apk::a64::register_use(word);
        if (use.reads != 0 || use.writes != 0) {
            const auto detail = std::format("{:08x} reads {} writes {}", word, registers(use.reads), registers(use.writes));
            apk::testing::fail(std::source_location::current(), "", detail.c_str());
            return;
        }
    }
}

// --- Comparison with llvm-objdump ------------------------------------------------------------

/// SVE (op1 = 0010) and SME (op0 = 1, op1 = 0000) are left out on purpose; see a64.h.
bool is_sve_or_sme(std::uint32_t word) {
    const auto op1 = (word >> 25) & 0xf;
    return op1 == 0b0010 || (op1 == 0 && (word >> 31) != 0);
}

/// Random words, and as many again with one of the register fields set to 18.
std::vector<std::uint32_t> random_words(std::size_t count) {
    Random random;
    std::vector<std::uint32_t> words;
    constexpr unsigned field_shifts[] = {0, 5, 10, 16};
    while (words.size() < count) {
        auto word = random();
        if (words.size() % 2 == 1) {
            const auto shift = field_shifts[random() % 4];
            word = (word & ~(0x1fu << shift)) | (18u << shift);
        }
        if (!is_sve_or_sme(word)) words.push_back(word);
    }
    return words;
}

/// The words of the executable sections of an ELF file, for real compiler output.
std::vector<std::uint32_t> code_words(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    const std::span bytes(reinterpret_cast<const std::byte *>(raw.data()), raw.size());
    std::vector<std::uint32_t> words;
    for (const auto &section : apk::elf::read_sections(bytes)) {
        if ((section.flags & apk::elf::section_executable) == 0 || section.type == apk::elf::section_type_nobits) {
            continue;
        }
        for (std::uint64_t at = 0; at + 4 <= section.size; at += 4) {
            std::uint32_t word;
            std::memcpy(&word, raw.data() + section.offset + at, sizeof word);
            if (!is_sve_or_sme(word)) words.push_back(word);
        }
    }
    return words;
}

/// An ELF file with `words` as its .text, for llvm-objdump.
void write_elf(const std::filesystem::path &path, const std::vector<std::uint32_t> &words) {
    using namespace apk::testing;
    constexpr std::size_t text = 0x1000;
    ElfBuilder builder;
    builder.bytes.resize(text + 4 * words.size());
    std::memcpy(builder.bytes.data() + text, words.data(), 4 * words.size());
    builder.sections({{".text", sht_progbits, shf_alloc | shf_execinstr, text, text, 4 * words.size()}});
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char *>(builder.bytes.data()), static_cast<std::streamsize>(builder.bytes.size()));
}

bool is_word_character(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

/// General-purpose registers named in the operands: x0 to x30 and w0 to w30, as whole words.
std::vector<unsigned> named_registers(std::string_view operands) {
    std::vector<unsigned> found;
    for (std::size_t at = 0; at < operands.size(); ++at) {
        if ((operands[at] != 'x' && operands[at] != 'w') || (at > 0 && is_word_character(operands[at - 1]))) continue;
        std::size_t end = at + 1;
        unsigned number = 0;
        while (end < operands.size() && end - at <= 2 && operands[end] >= '0' && operands[end] <= '9') {
            number = number * 10 + static_cast<unsigned>(operands[end] - '0');
            ++end;
        }
        if (end == at + 1 || (end < operands.size() && is_word_character(operands[end])) || number > 30) continue;
        found.push_back(number);
    }
    return found;
}

std::uint32_t mask_of(std::initializer_list<unsigned> registers) {
    std::uint32_t mask = 0;
    for (const auto reg : registers) mask |= 1u << reg;
    return mask;
}

/// What the disassembly does not show: the registers some instructions use implicitly, and the
/// register of TLBI operations without an address. LLVM prints those without their Rt field; the
/// decoder counts it as read, which is the safe side.
std::uint32_t implicit_registers(std::uint32_t word, std::string_view mnemonic, std::string_view operands,
                                 const std::vector<unsigned> &named) {
    constexpr unsigned lr = 30;
    if (mnemonic == "bl" || mnemonic.starts_with("blr")) return mask_of({lr});
    if (mnemonic == "ret" && named.empty()) return mask_of({lr});
    if (mnemonic.starts_with("reta")) return mask_of({lr});
    if (mnemonic == "xpaclri") return mask_of({lr});
    if (mnemonic.ends_with("1716")) return mask_of({16, 17});
    if (mnemonic.ends_with("171615")) return mask_of({15, 16, 17});
    for (const auto *name : {"paciaz", "pacibz", "autiaz", "autibz", "paciasp", "pacibsp", "autiasp", "autibsp"}) {
        if (mnemonic == name) return mask_of({lr});
    }
    if (mnemonic.ends_with("sppc") || mnemonic.ends_with("sppcr")) return mask_of({lr});
    if (mnemonic == "tlbi" && named.empty() && (word & 0x1f) != 31) return 1u << (word & 0x1f);
    // LS64: eight consecutive registers from the last one before the address.
    if (mnemonic == "ld64b" || mnemonic == "st64b" || mnemonic == "st64bv" || mnemonic == "st64bv0") {
        const auto before = named_registers(operands.substr(0, operands.find('[')));
        if (before.empty()) return 0;
        std::uint32_t mask = 0;
        for (unsigned reg = before.back(); reg < before.back() + 8 && reg < 31; ++reg) mask |= 1u << reg;
        return mask;
    }
    return 0;
}

struct Line {
    std::uint64_t address;
    std::string text;
};

/// Runs llvm-objdump on `path` and returns one line per instruction.
std::vector<Line> disassemble(const std::string &objdump, const std::filesystem::path &path) {
    const auto command = std::format("'{}' -d --disassemble-zeroes --no-show-raw-insn '{}'", objdump, path.string());
    FILE *pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) throw std::runtime_error("cannot run " + command);
    std::vector<Line> lines;
    char buffer[512];
    while (std::fgets(buffer, sizeof buffer, pipe) != nullptr) {
        std::string_view line(buffer);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || line.empty() || line[0] != ' ') continue;
        std::uint64_t address = 0;
        bool is_address = colon > 0;
        for (const char c : line.substr(0, colon)) {
            if (c == ' ') continue;
            const auto digit = std::string_view("0123456789abcdef").find(c);
            if (digit == std::string_view::npos) {
                is_address = false;
                break;
            }
            address = address * 16 + digit;
        }
        if (!is_address) continue;
        auto text = line.substr(colon + 1);
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
        while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.remove_suffix(1);
        lines.push_back({address, std::string(text)});
    }
    if (pclose(pipe) != 0) throw std::runtime_error("llvm-objdump failed: " + command);
    return lines;
}

/// `text` with the general-purpose register `from` renamed to `to`, as whole words: x and w forms.
std::string renamed(std::string_view text, unsigned from, unsigned to) {
    std::string result;
    for (std::size_t at = 0; at < text.size();) {
        const bool starts = (text[at] == 'x' || text[at] == 'w') && (at == 0 || !is_word_character(text[at - 1]));
        const auto digits = std::to_string(from);
        if (starts && text.substr(at + 1, digits.size()) == digits &&
            (at + 1 + digits.size() == text.size() || !is_word_character(text[at + 1 + digits.size()]))) {
            result += text[at];
            result += std::to_string(to);
            at += 1 + digits.size();
        } else {
            result += text[at++];
        }
    }
    return result;
}

/// `text` with each run of spaces and tabs as one space.
std::string squeezed(std::string_view text) {
    std::string result;
    for (const char c : text) {
        const bool blank = c == ' ' || c == '\t';
        if (blank && !result.empty() && result.back() == ' ') continue;
        result += blank ? ' ' : c;
    }
    return result;
}

/// The lowest register the word does not use, other than `avoid`: a replacement for x18.
unsigned free_register(std::uint32_t word, unsigned avoid) {
    const auto use = apk::a64::register_use(word);
    for (unsigned reg = 0; reg < 31; ++reg) {
        if (reg != avoid && ((use.reads | use.writes) & (1u << reg)) == 0) return reg;
    }
    return 31;
}

/// Substitutes x18 in every word that uses it and compares the disassembly of the result with the
/// original one, x18 renamed. The words keep their addresses, so PC-relative targets are the same.
int compare_substitutions(const std::string &objdump, const std::filesystem::path &scratch,
                          const std::vector<std::uint32_t> &words, const std::vector<Line> &lines) {
    std::vector<std::uint32_t> substituted = words;
    std::map<std::size_t, unsigned> replacement;  // index -> the register that replaces x18
    std::map<std::string, std::size_t> refused_by_mnemonic;
    std::map<std::size_t, std::string_view> original_text;
    for (const auto &line : lines) {
        if (line.address < 0x1000 || (line.address - 0x1000) % 4 != 0) continue;
        const auto index = (line.address - 0x1000) / 4;
        if (index >= words.size()) continue;
        const auto use = apk::a64::register_use(words[index]);
        if (((use.reads | use.writes) & (1u << 18)) == 0) continue;
        const auto to = free_register(words[index], 18);
        if (const auto word = apk::a64::substitute(words[index], 18, to)) {
            substituted[index] = *word;
            replacement[index] = to;
            original_text[index] = line.text;
        } else {
            const std::string_view text(line.text);
            const auto mnemonic = std::string(text.substr(0, text.find_first_of("\t ")));
            if (++refused_by_mnemonic[mnemonic] <= 2 && std::getenv("A64_SHOW_REFUSED") != nullptr) {
                std::fprintf(stderr, "refused %08x  %s\n", words[index], line.text.c_str());
            }
        }
    }
    const auto path = scratch / "substituted.elf";
    write_elf(path, substituted);
    std::size_t mismatches = 0;
    for (const auto &line : disassemble(objdump, path)) {
        if (line.address < 0x1000 || (line.address - 0x1000) % 4 != 0) continue;
        const auto index = (line.address - 0x1000) / 4;
        const auto found = replacement.find(index);
        if (found == replacement.end()) continue;
        // The comment column moves when the register name gets shorter or longer.
        const auto expected = squeezed(renamed(original_text[index], 18, found->second));
        if (squeezed(line.text) == expected) continue;
        if (++mismatches <= 40) {
            std::fprintf(stderr, "%08x -> %08x  %s, expected %s\n", words[index], substituted[index], line.text.c_str(),
                         expected.c_str());
        }
    }
    std::size_t refused = 0;
    for (const auto &[mnemonic, count] : refused_by_mnemonic) refused += count;
    std::printf("substituted x18 in %zu words, %zu mismatches; refused %zu:", replacement.size(), mismatches, refused);
    for (const auto &[mnemonic, count] : refused_by_mnemonic) std::printf(" %s %zu", mnemonic.c_str(), count);
    std::printf("\n");
    return mismatches == 0 ? 0 : 1;
}

int compare_with_objdump(const std::string &objdump, const std::filesystem::path &scratch,
                         const std::filesystem::path &fixtures) {
    auto words = random_words(400'000);
    std::size_t real_words = 0;
    for (const char *name : {"hello", "raw-hello"}) {
        if (!fixtures.empty() && std::filesystem::exists(fixtures / name)) {
            const auto code = code_words(fixtures / name);
            words.insert(words.end(), code.begin(), code.end());
            real_words += code.size();
        }
    }
    std::filesystem::create_directories(scratch);
    const auto path = scratch / "words.elf";
    write_elf(path, words);
    const auto lines = disassemble(objdump, path);

    std::size_t compared = 0, unknown = 0, mismatches = 0, unallocated = 0;
    std::map<std::string, std::size_t> mismatches_by_mnemonic;
    for (const auto &line : lines) {
        if (line.address < 0x1000 || (line.address - 0x1000) % 4 != 0) continue;
        const auto index = (line.address - 0x1000) / 4;
        if (index >= words.size()) continue;
        const auto word = words[index];
        // Arm leaves the 128-bit system words with L set and op0 0x, or L clear and op0 00, unallocated (MRRS and
        // MSRR take op0 = 1:o0, SYSP op0 = 01); llvm-objdump still prints them as MRRS or MSRR with an S0_ or S1_
        // register. The decoder gives them no register (data, a SIGILL if run), so they are counted, not compared.
        if ((word >> 22) == 0b1101010101u) {
            const bool load = ((word >> 21) & 1u) != 0;
            const auto op0 = (word >> 19) & 3u;
            if (load ? op0 < 2 : op0 == 0) {
                ++unallocated;
                continue;
            }
        }
        const std::string_view text(line.text);
        const auto tab = text.find_first_of("\t ");
        const auto mnemonic = text.substr(0, tab);
        const auto operands = tab == std::string_view::npos ? std::string_view() : text.substr(tab + 1);
        const auto named = named_registers(operands.substr(0, operands.find("//")));

        std::uint32_t expected = implicit_registers(word, mnemonic, operands, named);
        for (const auto reg : named) expected |= 1u << reg;
        const auto use = apk::a64::register_use(word);
        ++compared;
        if (mnemonic == "<unknown>") ++unknown;
        if ((use.reads | use.writes) == expected) continue;

        ++mismatches;
        ++mismatches_by_mnemonic[std::string(mnemonic)];
        if (mismatches <= 40) {
            std::fprintf(stderr, "%08x  %-40s decoder %s / %s, disassembly %s\n", word, line.text.c_str(),
                         registers(use.reads).c_str(), registers(use.writes).c_str(), registers(expected).c_str());
        }
    }
    std::printf("compared %zu words (%zu from the fixtures, %zu unknown to llvm-objdump, %zu unallocated 128-bit "
                "system words left out), %zu mismatches\n",
                compared, real_words, unknown, unallocated, mismatches);
    if (mismatches > 0) {
        for (const auto &[mnemonic, count] : mismatches_by_mnemonic) std::fprintf(stderr, "  %6zu  %s\n", count, mnemonic.c_str());
    }
    if (compared != words.size()) {
        std::fprintf(stderr, "llvm-objdump printed %zu of %zu words\n", compared, words.size());
        return 1;
    }
    const auto substitutions = compare_substitutions(objdump, scratch, words, lines);
    return mismatches == 0 && substitutions == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc == 1) return apk::testing::run_all();
    if (argc < 3 || argc > 4) {
        std::fprintf(stderr, "usage: a64_test [<llvm-objdump> <scratch-directory> [<fixtures-directory>]]\n");
        return 2;
    }
    if (std::string_view(argv[1]).empty()) {
        std::printf("no llvm-objdump found, skipped\n");
        return 77;
    }
    try {
        return compare_with_objdump(argv[1], argv[2], argc == 4 ? argv[3] : "");
    } catch (const std::exception &error) {
        std::fprintf(stderr, "a64_test: %s\n", error.what());
        return 1;
    }
}
