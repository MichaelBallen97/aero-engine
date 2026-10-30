// tests/script_bytecode_format_test.cpp -- task 4.3.2: the bytecode format head, the load-time rename and the read
// headroom rule (BY1-BY7). A TU of aero_tests, which supplies main() from test_main.cpp.
//
// Tier-0 through aero::script_internal: no VM, no backend call. Every synthetic head this file renames is built
// BY THIS FILE, byte by byte, from the format's own layout (version byte, checksum word, atom count, the name atom)
// and sealed with bytecodeChecksum, so each refusal arm mutates exactly ONE field of something already valid.
#include <aero/script/internal/bytecode_format.hpp>
#include <aero/script/internal/heap_accounting.hpp>
#include <aero/script/internal/module_graph.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <string_view>
// <ostream> is load-bearing on MSVC: doctest stringifies the std::string_view operands of the CHECKs below through
// operator<<(std::ostream&, std::string_view), which needs <ostream> to complete std::basic_ostream.
#include <ostream>
#include <vector>

namespace {

using engine::script::internal::BYTECODE_CHECKSUM_BYPASS;
using engine::script::internal::BYTECODE_READ_FIXED_BYTES;
using engine::script::internal::bytecodeChecksum;
using engine::script::internal::BytecodeHeadStatus;
using engine::script::internal::bytecodeHeadStatusText;
using engine::script::internal::bytecodeReadHeadroom;
using engine::script::internal::COOK_MODULE_NAME;
using engine::script::internal::COOK_PROBE_NAME;
using engine::script::internal::HEAP_HEADER_BYTES;
using engine::script::internal::HeapAccounting;
using engine::script::internal::renameBytecodeModule;
using engine::script::internal::RenamedBytecode;
using engine::script::internal::SCRIPT_BYTECODE_VERSION;

constexpr std::uint32_t MULTIPLIER = 0x9e370001U;

[[nodiscard]] std::vector<std::byte> bytesOf(std::initializer_list<unsigned> values) {
    std::vector<std::byte> out;
    for (const unsigned v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

// The checksum of `payload` written independently of the engine's copy, so a slip in one cannot hide in the other:
// whole words while i + 4 < n, then a switch on the remainder with arms 3, 2, 1 and no arm for 4 (quickjs.c:37806).
[[nodiscard]] std::uint32_t referenceChecksum(const std::vector<std::byte>& body) {
    std::uint32_t h = 0;
    std::size_t i = 0;
    const std::size_t n = body.size();
    while (i + 4 < n) {
        std::uint32_t word = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            word |= static_cast<std::uint32_t>(body[i + k]) << (8U * k);
        }
        h += word;
        h *= MULTIPLIER;
        i += 4;
    }
    std::uint32_t tail = 0;
    switch (n - i) {
        case 3:
            tail |= static_cast<std::uint32_t>(body[i + 2]) << 16U;
            [[fallthrough]];
        case 2:
            tail |= static_cast<std::uint32_t>(body[i + 1]) << 8U;
            [[fallthrough]];
        case 1:
            tail |= static_cast<std::uint32_t>(body[i]);
            break;
        default:
            break;
    }
    h += tail;
    h *= MULTIPLIER;
    return h;
}

void putWord(std::vector<std::byte>& bytes, std::uint32_t value) {
    for (std::size_t k = 0; k < 4; ++k) {
        bytes[1 + k] = static_cast<std::byte>((value >> (8U * k)) & 0xFFU);
    }
}

void seal(std::vector<std::byte>& bytes) {
    const std::vector<std::byte> body(bytes.begin() + 5, bytes.end());
    putWord(bytes, referenceChecksum(body));
}

// version, checksum word (patched by seal), `count` atoms announced, then the name atom (type 1, length word, name),
// then `tail` as the rest of the module (opaque to the rename).
[[nodiscard]] std::vector<std::byte> head(std::string_view name, std::size_t tailBytes, unsigned count = 1,
                                          unsigned type = 1) {
    std::vector<std::byte> out = bytesOf({SCRIPT_BYTECODE_VERSION, 0, 0, 0, 0, count, type});
    std::size_t word = name.size() << 1U;
    while (word >= 0x80U) {
        out.push_back(static_cast<std::byte>((word & 0x7FU) | 0x80U));
        word >>= 7U;
    }
    out.push_back(static_cast<std::byte>(word));
    for (const char c : name) {
        out.push_back(static_cast<std::byte>(c));
    }
    for (std::size_t i = 0; i < tailBytes; ++i) {
        out.push_back(static_cast<std::byte>(0xA0U + (i * 7U) % 40U));
    }
    seal(out);
    return out;
}

[[nodiscard]] std::span<const std::byte> view(const std::vector<std::byte>& v) { return {v.data(), v.size()}; }

}  // namespace

TEST_CASE("script: the checksum mirrors the backend's, the multiple-of-four gap included (BY1)") {
    CHECK(bytecodeChecksum({}) == 0U);
    const std::vector<std::byte> one = bytesOf({1});
    CHECK(bytecodeChecksum(view(one)) == 0x9e370001U);

    // Lengths 4, 8 and 12: the last four bytes never enter the sum; any earlier byte does.
    for (const std::size_t n : {std::size_t{4}, std::size_t{8}, std::size_t{12}}) {
        std::vector<std::byte> base(n);
        for (std::size_t i = 0; i < n; ++i) {
            base[i] = static_cast<std::byte>(i * 5U + 3U);
        }
        const std::uint32_t reference = bytecodeChecksum(view(base));
        for (std::size_t i = 0; i < n; ++i) {
            std::vector<std::byte> changed = base;
            changed[i] ^= std::byte{0x5A};
            if (i + 4 >= n) {
                CHECK(bytecodeChecksum(view(changed)) == reference);
            } else {
                CHECK(bytecodeChecksum(view(changed)) != reference);
            }
        }
    }
    // Lengths 5, 6 and 7: every byte matters.
    for (const std::size_t n : {std::size_t{5}, std::size_t{6}, std::size_t{7}}) {
        std::vector<std::byte> base(n);
        for (std::size_t i = 0; i < n; ++i) {
            base[i] = static_cast<std::byte>(i * 11U + 1U);
        }
        const std::uint32_t reference = bytecodeChecksum(view(base));
        for (std::size_t i = 0; i < n; ++i) {
            std::vector<std::byte> changed = base;
            changed[i] ^= std::byte{0x21};
            CHECK(bytecodeChecksum(view(changed)) != reference);
        }
    }
    // The independent reference agrees on a spread of lengths (anti-vacuity: the two copies are not one).
    for (std::size_t n = 1; n <= 40; ++n) {
        std::vector<std::byte> body(n);
        for (std::size_t i = 0; i < n; ++i) {
            body[i] = static_cast<std::byte>((i * 37U + n) & 0xFFU);
        }
        CHECK(bytecodeChecksum(view(body)) == referenceChecksum(body));
    }
    // Little-endian word loads: the same bytes in the other order are another sum.
    CHECK(bytecodeChecksum(view(bytesOf({1, 0, 0, 0, 0}))) != bytecodeChecksum(view(bytesOf({0, 0, 0, 1, 0}))));
}

TEST_CASE("script: renaming refuses every malformed head for its own reason (BY2)") {
    const std::string expected{COOK_MODULE_NAME};
    const std::string target{COOK_PROBE_NAME};
    const std::vector<std::byte> good = head(expected, 20);
    {
        const RenamedBytecode ok = renameBytecodeModule(view(good), expected, target);
        CHECK(ok.status == BytecodeHeadStatus::Ok);
        CHECK(!ok.bytes.empty());
    }
    // Every truncation of the valid head, through the end of the name atom, is Truncated or a checksum refusal --
    // never a read past the end and never Ok. Cuts below five bytes are Truncated outright.
    const std::size_t nameEnd = 7 + 1 + expected.size();
    for (std::size_t cut = 0; cut < nameEnd; ++cut) {
        std::vector<std::byte> truncated(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(cut));
        if (cut >= 5) {
            seal(truncated);  // a truncation the checksum agrees with: only the format walk can refuse it
        }
        const RenamedBytecode r = renameBytecodeModule(view(truncated), expected, target);
        CHECK(r.status == BytecodeHeadStatus::Truncated);
        CHECK(r.bytes.empty());
    }
    {
        std::vector<std::byte> v25 = good;
        v25[0] = std::byte{25};
        CHECK(renameBytecodeModule(view(v25), expected, target).status == BytecodeHeadStatus::WrongVersion);
        std::vector<std::byte> v27 = good;
        v27[0] = std::byte{27};
        CHECK(renameBytecodeModule(view(v27), expected, target).status == BytecodeHeadStatus::WrongVersion);
    }
    {
        std::vector<std::byte> bypass = good;
        putWord(bypass, BYTECODE_CHECKSUM_BYPASS);
        CHECK(renameBytecodeModule(view(bypass), expected, target).status == BytecodeHeadStatus::ChecksumBypass);
    }
    {
        std::vector<std::byte> flipped = good;
        flipped[9] ^= std::byte{1};
        CHECK(renameBytecodeModule(view(flipped), expected, target).status == BytecodeHeadStatus::ChecksumMismatch);
    }
    CHECK(renameBytecodeModule(view(head(expected, 20, 0)), expected, target).status == BytecodeHeadStatus::NoAtoms);
    CHECK(renameBytecodeModule(view(head(expected, 20, 1, 0)), expected, target).status ==
          BytecodeHeadStatus::NameNotAString);
    // Types 2 (a global symbol) and 3 (a symbol) are atoms the backend reads, carrying the expected name as a narrow
    // string: only the type byte can refuse them, so a check that refuses predefined atoms alone lets them through.
    for (const unsigned symbolType : {2U, 3U}) {
        CAPTURE(symbolType);
        const std::vector<std::byte> symbol = head(expected, 20, 1, symbolType);
        CHECK(renameBytecodeModule(view(symbol), expected, target).status == BytecodeHeadStatus::NameNotAString);
    }
    {
        // A wide string (low bit of the length word set), sealed so only the format walk can refuse it.
        std::vector<std::byte> wide = good;
        wide[7] = static_cast<std::byte>(static_cast<unsigned>(wide[7]) | 1U);
        seal(wide);
        CHECK(renameBytecodeModule(view(wide), expected, target).status == BytecodeHeadStatus::NameNotAString);
    }
    {
        const std::vector<std::byte> other = head("res://?other-x", 20);
        CHECK(renameBytecodeModule(view(other), expected, target).status == BytecodeHeadStatus::NameNotPlaceholder);
        // Same length, one different character.
        const std::vector<std::byte> near = head("res://?cookeD", 20);
        CHECK(renameBytecodeModule(view(near), expected, target).status == BytecodeHeadStatus::NameNotPlaceholder);
    }
    {
        // A six-byte continuation run in the atom count, then in the name's length word.
        std::vector<std::byte> longCount =
            bytesOf({SCRIPT_BYTECODE_VERSION, 0, 0, 0, 0, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x01, 0x01, 0x00});
        seal(longCount);
        CHECK(renameBytecodeModule(view(longCount), expected, target).status == BytecodeHeadStatus::Truncated);
        std::vector<std::byte> longWord =
            bytesOf({SCRIPT_BYTECODE_VERSION, 0, 0, 0, 0, 1, 1, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x01});
        seal(longWord);
        CHECK(renameBytecodeModule(view(longWord), expected, target).status == BytecodeHeadStatus::Truncated);
    }
    // The bypass is checked before the checksum: a payload carrying FF FF FF FF is ChecksumBypass whatever its body
    // sums to (so it is reachable at all).
    CHECK(bytecodeHeadStatusText(BytecodeHeadStatus::Ok).empty());
}

TEST_CASE("script: renaming rewrites the name and the checksum and nothing else (BY3)") {
    const std::string expected{COOK_MODULE_NAME};
    for (const std::size_t length : {std::size_t{62}, std::size_t{64}}) {
        const std::string name = "res://" + std::string(length - 6, 'n');
        REQUIRE(name.size() == length);
        const std::vector<std::byte> input = head(expected, 33);
        const RenamedBytecode out = renameBytecodeModule(view(input), expected, name);
        REQUIRE(out.status == BytecodeHeadStatus::Ok);
        // The length word is one byte up to 63 characters (126 < 128 for 62; 128 needs two for 64).
        const std::size_t lengthWordBytes = length == 62 ? 1U : 2U;
        const std::size_t afterName = 7 + lengthWordBytes + name.size();
        REQUIRE(out.bytes.size() == afterName + 33);
        // The bytes before the length word (version, atom count, type) are the input's; the checksum is new.
        CHECK(out.bytes[0] == input[0]);
        CHECK(out.bytes[5] == input[5]);
        CHECK(out.bytes[6] == input[6]);
        const std::size_t inputAfterName = 7 + 1 + expected.size();
        for (std::size_t i = 0; i < 33; ++i) {
            CHECK(out.bytes[afterName + i] == input[inputAfterName + i]);
        }
        if (length == 62) {
            CHECK(out.bytes[7] == static_cast<std::byte>(124));
        } else {
            CHECK(out.bytes[7] == static_cast<std::byte>(0x80));
            CHECK(out.bytes[8] == static_cast<std::byte>(1));
        }
        const std::vector<std::byte> body(out.bytes.begin() + 5, out.bytes.end());
        std::uint32_t stored = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            stored |= static_cast<std::uint32_t>(out.bytes[1 + k]) << (8U * k);
        }
        CHECK(stored == bytecodeChecksum(view(body)));
        CHECK(stored == referenceChecksum(body));
        // And back again: the input, byte for byte.
        const RenamedBytecode back = renameBytecodeModule(view(out.bytes), name, expected);
        REQUIRE(back.status == BytecodeHeadStatus::Ok);
        CHECK(back.bytes == input);
    }
}

TEST_CASE("script: a new name that is not printable ASCII is refused and the payload never touched (BY4)") {
    const std::string expected{COOK_MODULE_NAME};
    const std::vector<std::byte> input = head(expected, 20);
    const std::vector<std::byte> copy = head(expected, 20);  // built apart: head() is deterministic
    const std::string embeddedNul{"res://a\0b", 9};
    const std::string utf8{"res://\xC3\xBC"};
    CHECK(renameBytecodeModule(view(input), expected, "").status == BytecodeHeadStatus::InvalidNewName);
    CHECK(renameBytecodeModule(view(input), expected, embeddedNul).status == BytecodeHeadStatus::InvalidNewName);
    CHECK(renameBytecodeModule(view(input), expected, utf8).status == BytecodeHeadStatus::InvalidNewName);
    const RenamedBytecode refused = renameBytecodeModule(view(input), expected, utf8);
    CHECK(refused.bytes.empty());
    CHECK(input == copy);
    // The text of every refusal is non-empty except Ok's, and distinct.
    CHECK(bytecodeHeadStatusText(BytecodeHeadStatus::InvalidNewName) ==
          std::string_view{"the module's name is not printable ASCII"});
    CHECK(bytecodeHeadStatusText(BytecodeHeadStatus::Truncated) ==
          std::string_view{"its bytecode ends inside the format head"});
    CHECK(bytecodeHeadStatusText(BytecodeHeadStatus::WrongVersion) ==
          std::string_view{"its payload's bytecode version byte is not 26"});
    CHECK(bytecodeHeadStatusText(BytecodeHeadStatus::ChecksumBypass) ==
          std::string_view{"its backend checksum is the bypass value FF FF FF FF"});
    CHECK(bytecodeHeadStatusText(BytecodeHeadStatus::ChecksumMismatch) ==
          std::string_view{"its backend checksum does not match its bytes"});
    CHECK(bytecodeHeadStatusText(BytecodeHeadStatus::NoAtoms) == std::string_view{"its atom table is empty"});
    CHECK(bytecodeHeadStatusText(BytecodeHeadStatus::NameNotAString) ==
          std::string_view{"its module name is not a narrow string atom"});
    CHECK(bytecodeHeadStatusText(BytecodeHeadStatus::NameNotPlaceholder) ==
          std::string_view{"its module name is not the cook's placeholder (it was not written by this engine's cook)"});
}

TEST_CASE("script: the read headroom is a fixed part plus forty-eight per payload byte, saturating (BY5)") {
    constexpr std::size_t MIB = std::size_t{1024} * 1024U;
    CHECK(bytecodeReadHeadroom(0) == 65536U);
    CHECK(bytecodeReadHeadroom(1) == 65584U);
    CHECK(bytecodeReadHeadroom(MIB) == 65536U + 48U * MIB);
    constexpr std::size_t MAX = std::numeric_limits<std::size_t>::max();
    CHECK(bytecodeReadHeadroom(MAX / 48U) == MAX);
    CHECK(bytecodeReadHeadroom(MAX) == MAX);
    CHECK(bytecodeReadHeadroom(MAX - 1U) == MAX);
    // The largest payload that does not saturate.
    const std::size_t edge = (MAX - BYTECODE_READ_FIXED_BYTES) / 48U;
    CHECK(bytecodeReadHeadroom(edge) == BYTECODE_READ_FIXED_BYTES + 48U * edge);
    CHECK(bytecodeReadHeadroom(edge + 1U) == MAX);
}

TEST_CASE("script: admitting headroom counts a refusal as a heap refusal, never a growth one (BY6)") {
    constexpr std::size_t LIMIT = std::size_t{1024} * 1024U;
    HeapAccounting heap{LIMIT};
    void* block = heap.allocate(1000);
    REQUIRE(block != nullptr);
    const std::size_t inUse = heap.bytesInUse();
    REQUIRE(inUse == 1000U + HEAP_HEADER_BYTES);
    const std::size_t room = LIMIT - inUse;

    CHECK(heap.admitsHeadroom(room));
    CHECK(heap.refusals() == 0U);
    CHECK(!heap.admitsHeadroom(room + 1U));
    CHECK(heap.refusals() == 1U);
    CHECK(heap.growthRefusals() == 0U);
    CHECK(!heap.admitsHeadroom(std::numeric_limits<std::size_t>::max()));
    CHECK(heap.refusals() == 2U);
    CHECK(heap.growthRefusals() == 0U);
    CHECK(heap.bytesInUse() == inUse);
    CHECK(heap.liveBlocks() == 1U);

    // A grace window admits bytes past the limit; the guard reads none of that: once inUse is past the limit, every
    // request but zero refuses, and the guard spends no decision of the window.
    heap.openGraceWindow(4, LIMIT);
    void* extra = heap.allocate(LIMIT - HEAP_HEADER_BYTES - 100U);  // the window lets inUse run past the limit
    REQUIRE(extra != nullptr);
    REQUIRE(heap.bytesInUse() > LIMIT);
    const std::uint64_t before = heap.refusals();
    CHECK(!heap.admitsHeadroom(1));
    CHECK(heap.refusals() == before + 1U);
    CHECK(heap.admitsHeadroom(0));
    CHECK(heap.refusals() == before + 1U);
    heap.release(extra);
    heap.release(block);
    CHECK(heap.liveBlocks() == 0U);
}

TEST_CASE("script: the cook's placeholders can be neither imported nor become a module's name (BY7)") {
    using engine::script::internal::decodeModuleName;
    using engine::script::internal::importerKey;
    using engine::script::internal::resolveModuleSpecifier;
    using engine::script::internal::SpecifierRefusal;

    CHECK(COOK_MODULE_NAME != COOK_PROBE_NAME);
    CHECK(COOK_MODULE_NAME.size() != COOK_PROBE_NAME.size());
    CHECK(!COOK_PROBE_NAME.starts_with(COOK_MODULE_NAME));
    CHECK(!COOK_MODULE_NAME.starts_with(COOK_PROBE_NAME));
    for (const std::string_view name : {COOK_MODULE_NAME, COOK_PROBE_NAME}) {
        for (const char c : name) {
            CHECK(static_cast<unsigned char>(c) >= 0x20U);
            CHECK(static_cast<unsigned char>(c) < 0x7FU);
        }
        CHECK(!decodeModuleName(name).has_value());
        CHECK(resolveModuleSpecifier(name, "").refusal == SpecifierRefusal::Reserved);
        CHECK(resolveModuleSpecifier(name, "res://scripts/a.js").refusal == SpecifierRefusal::Reserved);
        CHECK(!importerKey(name).has_value());
    }
}
