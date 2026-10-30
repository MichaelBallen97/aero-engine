// tests/cooked_script_test.cpp -- task 4.3.2: the .aeroscript container v1. A TU of aero_tests, which
// supplies main() from test_main.cpp -- do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// Tier-0: no GPU, no window, no disk, no script VM. Every buffer this file parses is built BY THIS FILE,
// byte by byte, from docs/09 section 16.1's own table, with the offsets spelled as literals, so a
// writer bug can never mask a parser bug and every refusal arm mutates exactly ONE field of something
// already valid. Only CK2 and CK3 read the writer's output.
#include <aero/assets/cooked_script.hpp>
#include <aero/core/content_hash.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <span>
#include <string>
#include <string_view>
// <ostream> is load-bearing on MSVC: doctest stringifies the std::string_view operands of the magic and
// label CHECKs through operator<<(std::ostream&, std::string_view), which needs <ostream> to complete
// std::basic_ostream. libc++ and libstdc++ are self-sufficient; only the Windows lane would fail.
#include <ostream>
#include <vector>

using engine::ContentHash;
using engine::Guid;
using engine::hashBytes;
using engine::assets::COOKED_SCRIPT_COOKER_VERSION;
using engine::assets::COOKED_SCRIPT_FORMAT_VERSION;
using engine::assets::COOKED_SCRIPT_HEADER_BYTES;
using engine::assets::COOKED_SCRIPT_KNOWN_STRIP_FLAGS;
using engine::assets::COOKED_SCRIPT_MAGIC;
using engine::assets::COOKED_SCRIPT_STRIP_SOURCE;
using engine::assets::CookedScriptInput;
using engine::assets::CookedScriptParseResult;
using engine::assets::CookedScriptStatus;
using engine::assets::cookedScriptStatusLabel;
using engine::assets::CookedScriptWriteResult;
using engine::assets::getU32;
using engine::assets::getU64;
using engine::assets::hasCookedScriptMagic;
using engine::assets::parseCookedScript;
using engine::assets::putU32;
using engine::assets::putU64;
using engine::assets::writeCookedScript;

namespace {

static_assert(COOKED_SCRIPT_HEADER_BYTES == 80);

constexpr Guid TEST_GUID{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};

// docs/09 section 16.1's offsets, as LITERALS: a golden that read them from detail::H_* could not see
// either copy move.
constexpr std::size_t AT_MAGIC = 0;
constexpr std::size_t AT_FORMAT_VERSION = 8;
constexpr std::size_t AT_COOKER_VERSION = 12;
constexpr std::size_t AT_GUID_HI = 16;
constexpr std::size_t AT_GUID_LO = 24;
constexpr std::size_t AT_BACKEND_RELEASE = 32;
constexpr std::size_t AT_BYTECODE_VERSION = 36;
constexpr std::size_t AT_STRIP_FLAGS = 40;
constexpr std::size_t AT_RESERVED_FLAGS = 44;
constexpr std::size_t AT_PAYLOAD_BYTES = 48;
constexpr std::size_t AT_PAYLOAD_HASH_HI = 56;
constexpr std::size_t AT_PAYLOAD_HASH_LO = 64;
constexpr std::size_t AT_RESERVED = 72;

constexpr std::array<std::byte, 8> MAGIC_BYTES{std::byte{0x00}, std::byte{0x41}, std::byte{0x45}, std::byte{0x52},
                                               std::byte{0x4F}, std::byte{0x53}, std::byte{0x43}, std::byte{0x52}};

[[nodiscard]] std::vector<std::byte> bytesOf(std::initializer_list<unsigned> values) {
    std::vector<std::byte> out;
    for (const unsigned v : values) {
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

// A well-formed container, built field by field with the hash from core.
[[nodiscard]] std::vector<std::byte> buildScript(std::span<const std::byte> payload, Guid guid = TEST_GUID,
                                                 std::uint32_t release = 0x00000F01U, std::uint32_t version = 26,
                                                 std::uint32_t stripFlags = 1) {
    std::vector<std::byte> bytes(COOKED_SCRIPT_HEADER_BYTES + payload.size());
    const std::span<std::byte> w(bytes);
    for (std::size_t i = 0; i < MAGIC_BYTES.size(); ++i) {
        w[AT_MAGIC + i] = MAGIC_BYTES[i];
    }
    putU32(w, AT_FORMAT_VERSION, 1);
    putU32(w, AT_COOKER_VERSION, 1);
    putU64(w, AT_GUID_HI, guid.hi);
    putU64(w, AT_GUID_LO, guid.lo);
    putU32(w, AT_BACKEND_RELEASE, release);
    putU32(w, AT_BYTECODE_VERSION, version);
    putU32(w, AT_STRIP_FLAGS, stripFlags);
    putU32(w, AT_RESERVED_FLAGS, 0);
    putU64(w, AT_PAYLOAD_BYTES, payload.size());
    const ContentHash hash = hashBytes(payload);
    putU64(w, AT_PAYLOAD_HASH_HI, hash.hi);
    putU64(w, AT_PAYLOAD_HASH_LO, hash.lo);
    putU64(w, AT_RESERVED, 0);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        w[COOKED_SCRIPT_HEADER_BYTES + i] = payload[i];
    }
    return bytes;
}

[[nodiscard]] std::vector<std::byte> referencePayload() {
    return bytesOf({0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87});
}

[[nodiscard]] std::vector<std::byte> buildReference() {
    const std::vector<std::byte> payload = referencePayload();
    return buildScript(payload);
}

// A 32-byte payload of distinct values, so a flip at any offset changes the hash.
[[nodiscard]] std::vector<std::byte> longPayload() {
    std::vector<std::byte> out(32);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::byte>((i * 7U) + 3U);
    }
    return out;
}

void flip(std::vector<std::byte>& bytes, std::size_t at) {
    bytes[at] = static_cast<std::byte>(static_cast<unsigned>(bytes[at]) ^ 0xFFU);
}

}  // namespace

TEST_CASE("script container: the identity constants and header offsets are pinned against literals (CK1)") {
    CHECK(COOKED_SCRIPT_HEADER_BYTES == 80);
    CHECK(COOKED_SCRIPT_FORMAT_VERSION == 1);
    CHECK(COOKED_SCRIPT_COOKER_VERSION == 1);
    CHECK(COOKED_SCRIPT_STRIP_SOURCE == 1);
    CHECK(COOKED_SCRIPT_KNOWN_STRIP_FLAGS == 1);
    CHECK(engine::assets::cooked_script_detail::H_MAGIC == 0);
    CHECK(engine::assets::cooked_script_detail::H_FORMAT_VERSION == 8);
    CHECK(engine::assets::cooked_script_detail::H_COOKER_VERSION == 12);
    CHECK(engine::assets::cooked_script_detail::H_GUID_HI == 16);
    CHECK(engine::assets::cooked_script_detail::H_GUID_LO == 24);
    CHECK(engine::assets::cooked_script_detail::H_BACKEND_RELEASE == 32);
    CHECK(engine::assets::cooked_script_detail::H_BYTECODE_VERSION == 36);
    CHECK(engine::assets::cooked_script_detail::H_STRIP_FLAGS == 40);
    CHECK(engine::assets::cooked_script_detail::H_RESERVED_FLAGS == 44);
    CHECK(engine::assets::cooked_script_detail::H_PAYLOAD_BYTES == 48);
    CHECK(engine::assets::cooked_script_detail::H_PAYLOAD_HASH_HI == 56);
    CHECK(engine::assets::cooked_script_detail::H_PAYLOAD_HASH_LO == 64);
    CHECK(engine::assets::cooked_script_detail::H_RESERVED == 72);
    REQUIRE(COOKED_SCRIPT_MAGIC.size() == 8);
    for (std::size_t i = 0; i < 8; ++i) {
        CHECK(static_cast<std::byte>(COOKED_SCRIPT_MAGIC[i]) == MAGIC_BYTES[i]);
    }
    CHECK(MAGIC_BYTES[0] == std::byte{0});
}

TEST_CASE("script container: a write equals the frozen 88-byte golden (CK2)") {
    const std::vector<std::byte> payload = referencePayload();
    CookedScriptInput input;
    input.sourceGuid = TEST_GUID;
    input.backendRelease = 0x00000F01U;
    input.bytecodeVersion = 26;
    input.stripFlags = 1;
    input.payload = payload;
    const CookedScriptWriteResult written = writeCookedScript(input);
    REQUIRE(written.status == CookedScriptStatus::Ok);
    CHECK(written.message.empty());

    // Hand-laid from docs/09 section 16.1, little-endian, every field checked against its row.
    const std::vector<std::byte> golden = bytesOf({
        0x00, 0x41, 0x45, 0x52, 0x4F, 0x53, 0x43, 0x52,  // 0   magic
        0x01, 0x00, 0x00, 0x00,                          // 8   formatVersion 1
        0x01, 0x00, 0x00, 0x00,                          // 12  cookerVersion 1
        0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01,  // 16  guid.hi
        0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE,  // 24  guid.lo
        0x01, 0x0F, 0x00, 0x00,                          // 32  backendRelease 0xF01
        0x1A, 0x00, 0x00, 0x00,                          // 36  bytecodeVersion 26
        0x01, 0x00, 0x00, 0x00,                          // 40  stripFlags 1
        0x00, 0x00, 0x00, 0x00,                          // 44  reservedFlags
        0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 48  payloadBytes 8
        0xC0, 0x06, 0xCA, 0x4D, 0xB3, 0x8A, 0x04, 0x64,  // 56  payloadHash.hi
        0x05, 0x2F, 0x1D, 0x4C, 0xA7, 0x55, 0x5A, 0xE8,  // 64  payloadHash.lo
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 72  reserved
        0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,  // 80  payload
    });
    REQUIRE(golden.size() == 88);
    CHECK(written.bytes == golden);
}

TEST_CASE("script container: parse(write(x)) returns every field and aliases the buffer (CK3)") {
    const std::vector<std::byte> payload = longPayload();
    CookedScriptInput input;
    input.sourceGuid = TEST_GUID;
    input.backendRelease = 0x00000F01U;
    input.bytecodeVersion = 26;
    input.stripFlags = 1;
    input.payload = payload;
    const CookedScriptWriteResult written = writeCookedScript(input);
    REQUIRE(written.status == CookedScriptStatus::Ok);

    const CookedScriptParseResult parsed = parseCookedScript(written.bytes);
    REQUIRE(parsed.status == CookedScriptStatus::Ok);
    CHECK(parsed.message.empty());
    CHECK(parsed.script.formatVersion == 1);
    CHECK(parsed.script.cookerVersion == 1);
    CHECK(parsed.script.sourceGuid == TEST_GUID);
    CHECK(parsed.script.backendRelease == 0x00000F01U);
    CHECK(parsed.script.bytecodeVersion == 26);
    CHECK(parsed.script.stripFlags == 1);
    CHECK(parsed.script.payloadBytes == 32);
    CHECK(parsed.script.payloadHash == hashBytes(payload));
    REQUIRE(parsed.script.payload.size() == 32);
    CHECK(parsed.script.payload.data() == written.bytes.data() + 80);
    CHECK(std::vector<std::byte>(parsed.script.payload.begin(), parsed.script.payload.end()) == payload);

    // A nil GUID is legal and round-trips.
    input.sourceGuid = Guid{};
    const CookedScriptWriteResult nilWritten = writeCookedScript(input);
    REQUIRE(nilWritten.status == CookedScriptStatus::Ok);
    const CookedScriptParseResult nilParsed = parseCookedScript(nilWritten.bytes);
    REQUIRE(nilParsed.status == CookedScriptStatus::Ok);
    CHECK(nilParsed.script.sourceGuid == Guid{});
    CHECK(getU64(nilWritten.bytes, 16) == 0);
    CHECK(getU64(nilWritten.bytes, 24) == 0);
}

TEST_CASE("script container: hasCookedScriptMagic decides on the first eight bytes only (CK4)") {
    CHECK(hasCookedScriptMagic(MAGIC_BYTES));
    const std::vector<std::byte> full = buildReference();
    CHECK(hasCookedScriptMagic(full));
    CHECK_FALSE(hasCookedScriptMagic(std::span<const std::byte>()));
    CHECK_FALSE(hasCookedScriptMagic(std::span<const std::byte>(MAGIC_BYTES).first(7)));
    const std::vector<std::byte> noNul = bytesOf({0x41, 0x45, 0x52, 0x4F, 0x53, 0x43, 0x52, 0x00});
    CHECK_FALSE(hasCookedScriptMagic(noNul));
    const std::vector<std::byte> noNulSeven = bytesOf({0x41, 0x45, 0x52, 0x4F, 0x53, 0x43, 0x52});
    CHECK_FALSE(hasCookedScriptMagic(noNulSeven));
    const std::vector<std::byte> bom = bytesOf({0xEF, 0xBB, 0xBF, 0x65, 0x78, 0x70, 0x6F, 0x72, 0x74});
    CHECK_FALSE(hasCookedScriptMagic(bom));
    const std::vector<std::byte> js = bytesOf({0x65, 0x78, 0x70, 0x6F, 0x72, 0x74, 0x20, 0x63, 0x6F, 0x6E});
    CHECK_FALSE(hasCookedScriptMagic(js));
    const std::vector<std::byte> wave = bytesOf({0x41, 0x45, 0x52, 0x4F, 0x57, 0x41, 0x56, 0x45});
    CHECK_FALSE(hasCookedScriptMagic(wave));
}

TEST_CASE("script container: every length below the header is TooSmall and names the length (CK5)") {
    const std::vector<std::byte> full = buildReference();
    for (std::size_t n = 0; n < COOKED_SCRIPT_HEADER_BYTES; ++n) {
        const CookedScriptParseResult parsed = parseCookedScript(std::span<const std::byte>(full).first(n));
        CHECK(parsed.status == CookedScriptStatus::TooSmall);
        CHECK(parsed.message.find(std::to_string(n) + " bytes") != std::string::npos);
        CHECK(parsed.message.find("80-byte header") != std::string::npos);
    }
}

TEST_CASE("script container: each magic byte flipped is BadMagic (CK6)") {
    for (std::size_t i = 0; i < 8; ++i) {
        std::vector<std::byte> bytes = buildReference();
        flip(bytes, AT_MAGIC + i);
        const CookedScriptParseResult parsed = parseCookedScript(bytes);
        CHECK(parsed.status == CookedScriptStatus::BadMagic);
        CHECK(parsed.message.find("magic") != std::string::npos);
    }
}

TEST_CASE("script container: formatVersion gates a parse and cookerVersion never does (CK7)") {
    for (const std::uint32_t v : {0U, 2U, 0xFFFFFFFFU}) {
        std::vector<std::byte> bytes = buildReference();
        putU32(bytes, AT_FORMAT_VERSION, v);
        const CookedScriptParseResult parsed = parseCookedScript(bytes);
        CHECK(parsed.status == CookedScriptStatus::UnsupportedVersion);
        CHECK(parsed.message.find(std::to_string(v)) != std::string::npos);
    }
    for (const std::uint32_t v : {0U, 2U, 0xFFFFFFFFU}) {
        std::vector<std::byte> bytes = buildReference();
        putU32(bytes, AT_COOKER_VERSION, v);
        const CookedScriptParseResult parsed = parseCookedScript(bytes);
        CHECK(parsed.status == CookedScriptStatus::Ok);
        CHECK(parsed.script.cookerVersion == v);
    }
}

TEST_CASE("script container: every byte of both reserved fields is refused alone (CK8)") {
    for (std::size_t i = 0; i < 4; ++i) {
        std::vector<std::byte> bytes = buildReference();
        bytes[AT_RESERVED_FLAGS + i] = std::byte{0x01};
        const CookedScriptParseResult parsed = parseCookedScript(bytes);
        CHECK(parsed.status == CookedScriptStatus::ReservedNotZero);
        CHECK(parsed.message.find("reserved flags") != std::string::npos);
    }
    for (std::size_t i = 0; i < 8; ++i) {
        std::vector<std::byte> bytes = buildReference();
        bytes[AT_RESERVED + i] = std::byte{0x01};
        const CookedScriptParseResult parsed = parseCookedScript(bytes);
        CHECK(parsed.status == CookedScriptStatus::ReservedNotZero);
        CHECK(parsed.message.find("offset 72") != std::string::npos);
    }
}

TEST_CASE("script container: unknown strip bits are refused by parse and by write (CK9)") {
    const std::vector<std::byte> payload = referencePayload();
    for (const std::uint32_t flags : {2U, 0x80000000U, 3U}) {
        std::vector<std::byte> bytes = buildScript(payload, TEST_GUID, 0xF01U, 26, 0);
        putU32(bytes, AT_STRIP_FLAGS, flags);
        const CookedScriptParseResult parsed = parseCookedScript(bytes);
        CHECK(parsed.status == CookedScriptStatus::UnknownFlags);
        CHECK(parsed.message.find("strip flags") != std::string::npos);

        CookedScriptInput input;
        input.stripFlags = flags;
        input.payload = payload;
        const CookedScriptWriteResult written = writeCookedScript(input);
        CHECK(written.status == CookedScriptStatus::UnknownFlags);
        CHECK(written.bytes.empty());
        CHECK(written.message == parsed.message);
    }
    for (const std::uint32_t flags : {0U, 1U}) {
        const std::vector<std::byte> bytes = buildScript(payload, TEST_GUID, 0xF01U, 26, flags);
        CHECK(parseCookedScript(bytes).status == CookedScriptStatus::Ok);
        CookedScriptInput input;
        input.stripFlags = flags;
        input.payload = payload;
        CHECK(writeCookedScript(input).status == CookedScriptStatus::Ok);
    }
}

TEST_CASE("script container: an empty payload is refused by parse and by write (CK10)") {
    std::vector<std::byte> bytes = buildReference();
    bytes.resize(COOKED_SCRIPT_HEADER_BYTES);
    putU64(bytes, AT_PAYLOAD_BYTES, 0);
    const CookedScriptParseResult parsed = parseCookedScript(bytes);
    CHECK(parsed.status == CookedScriptStatus::EmptyPayload);
    CHECK(parsed.message == "the header declares an empty payload");

    CookedScriptInput input;
    const CookedScriptWriteResult written = writeCookedScript(input);
    CHECK(written.status == CookedScriptStatus::EmptyPayload);
    CHECK(written.bytes.empty());
    CHECK(written.message == parsed.message);
}

TEST_CASE("script container: a payload size that is not exactly what follows is SizeMismatch (CK11)") {
    std::vector<std::byte> longer = buildReference();
    longer.push_back(std::byte{0});
    CHECK(parseCookedScript(longer).status == CookedScriptStatus::SizeMismatch);

    std::vector<std::byte> shorter = buildReference();
    shorter.pop_back();
    const CookedScriptParseResult parsed = parseCookedScript(shorter);
    CHECK(parsed.status == CookedScriptStatus::SizeMismatch);
    CHECK(parsed.message.find("8-byte payload") != std::string::npos);
    CHECK(parsed.message.find("7 bytes follow") != std::string::npos);

    // 80 + payloadBytes would wrap to 79 here and "match" a 79-byte buffer; the subtraction cannot.
    std::vector<std::byte> huge = buildReference();
    putU64(huge, AT_PAYLOAD_BYTES, std::numeric_limits<std::uint64_t>::max());
    CHECK(parseCookedScript(huge).status == CookedScriptStatus::SizeMismatch);
}

TEST_CASE("script container: every payload byte and every hash byte is covered by the hash (CK12)") {
    const std::vector<std::byte> payload = longPayload();
    const std::vector<std::byte> valid = buildScript(payload);
    REQUIRE(parseCookedScript(valid).status == CookedScriptStatus::Ok);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        std::vector<std::byte> bytes = valid;
        flip(bytes, COOKED_SCRIPT_HEADER_BYTES + i);
        const CookedScriptParseResult parsed = parseCookedScript(bytes);
        INFO("payload byte ", i);
        CHECK(parsed.status == CookedScriptStatus::HashMismatch);
    }
    // The last four bytes (offsets 28..31) are the ones the backend's own checksum never sees.
    for (std::size_t i = AT_PAYLOAD_HASH_HI; i < AT_RESERVED; ++i) {
        std::vector<std::byte> bytes = valid;
        flip(bytes, i);
        const CookedScriptParseResult parsed = parseCookedScript(bytes);
        INFO("hash byte ", i);
        CHECK(parsed.status == CookedScriptStatus::HashMismatch);
        CHECK(parsed.message.find("damaged") != std::string::npos);
    }
}

TEST_CASE("script container: the parse order reports the earliest failing check (CK13)") {
    std::vector<std::byte> futureAndDamaged = buildScript(longPayload());
    putU32(futureAndDamaged, AT_FORMAT_VERSION, 2);
    flip(futureAndDamaged, COOKED_SCRIPT_HEADER_BYTES + 3);
    CHECK(parseCookedScript(futureAndDamaged).status == CookedScriptStatus::UnsupportedVersion);

    std::vector<std::byte> reservedAndFlags = buildReference();
    putU32(reservedAndFlags, AT_RESERVED_FLAGS, 1);
    putU32(reservedAndFlags, AT_STRIP_FLAGS, 2);
    CHECK(parseCookedScript(reservedAndFlags).status == CookedScriptStatus::ReservedNotZero);

    std::vector<std::byte> flagsAndSize = buildReference();
    putU32(flagsAndSize, AT_STRIP_FLAGS, 2);
    flagsAndSize.push_back(std::byte{0});
    CHECK(parseCookedScript(flagsAndSize).status == CookedScriptStatus::UnknownFlags);

    std::vector<std::byte> sizeAndHash = buildReference();
    sizeAndHash.push_back(std::byte{0});
    flip(sizeAndHash, 81);
    CHECK(parseCookedScript(sizeAndHash).status == CookedScriptStatus::SizeMismatch);
}

TEST_CASE("script container: the nine status labels are distinct and non-empty (CK14)") {
    const std::array<CookedScriptStatus, 9> all{CookedScriptStatus::Ok,
                                                CookedScriptStatus::TooSmall,
                                                CookedScriptStatus::BadMagic,
                                                CookedScriptStatus::UnsupportedVersion,
                                                CookedScriptStatus::ReservedNotZero,
                                                CookedScriptStatus::UnknownFlags,
                                                CookedScriptStatus::EmptyPayload,
                                                CookedScriptStatus::SizeMismatch,
                                                CookedScriptStatus::HashMismatch};
    std::set<std::string_view> labels;
    for (const CookedScriptStatus status : all) {
        const std::string_view label = cookedScriptStatusLabel(status);
        CHECK_FALSE(label.empty());
        labels.insert(label);
    }
    CHECK(labels.size() == 9);
}
