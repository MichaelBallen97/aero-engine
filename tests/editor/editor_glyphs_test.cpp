// tests/editor/editor_glyphs_test.cpp -- task E.6.1: glyphs, icons and the string-literal policy (GL1-GL7).
// TIER 0, EVERY CONFIGURATION: both macro headers are pure and the policy scan reads source text, so
// nothing here needs a device, a window or generated meta. NO #if of any kind (the 3.6.3 rule). THIS FILE
// IS ASCII-ONLY: every non-ASCII byte a snippet needs is built from hex escapes at run time.
#include <aero/editor/asset_view.hpp>  // elideCaptionRight, CaptionLineFits (GL7)
#include <aero/editor/editor_glyphs.hpp>
#include <aero/editor/editor_icons.hpp>
#include <aero/editor/material_card.hpp>  // MATERIAL_DISPLAY_NAME_ELLIPSIS (GL7)
#include <aero/editor/text_file.hpp>      // readTextFile
#include <aero/reflect/json_reader.hpp>   // parseJson -- the reader editor_prefs.cpp uses (GL4)
#include <aero/reflect/json_value.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <ostream>  // MSVC: a CHECK over a std::string_view needs std::ostream (the 0.4.1 trap)
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace ed = engine::editor;

// U+2026 as bytes, built from escapes -- the raw byte a snippet must carry to prove the scan sees it.
const std::string ellipsisBytes = "\xE2\x80\xA6";

// ---- a strict UTF-8 decoder -------------------------------------------------------------------------
// Exactly ONE code point in `bytes`, or nullopt: refuses an overlong form, a surrogate, a value past
// U+10FFFF, a bad lead or continuation byte, and any trailing byte.
[[nodiscard]] std::optional<char32_t> decodeUtf8(std::string_view bytes) {
    if (bytes.empty()) {
        return std::nullopt;
    }
    const auto lead = static_cast<unsigned char>(bytes[0]);
    std::size_t length = 0;
    char32_t value = 0;
    char32_t minimum = 0;
    if (lead < 0x80U) {
        length = 1;
        value = lead;
    } else if ((lead & 0xE0U) == 0xC0U) {
        length = 2;
        value = lead & 0x1FU;
        minimum = 0x80U;
    } else if ((lead & 0xF0U) == 0xE0U) {
        length = 3;
        value = lead & 0x0FU;
        minimum = 0x800U;
    } else if ((lead & 0xF8U) == 0xF0U) {
        length = 4;
        value = lead & 0x07U;
        minimum = 0x10000U;
    } else {
        return std::nullopt;
    }
    if (bytes.size() != length) {
        return std::nullopt;
    }
    for (std::size_t i = 1; i < length; ++i) {
        const auto continuation = static_cast<unsigned char>(bytes[i]);
        if ((continuation & 0xC0U) != 0x80U) {
            return std::nullopt;
        }
        value = (value << 6U) | (continuation & 0x3FU);
    }
    if (value < minimum || value > 0x10FFFFU || (value >= 0xD800U && value <= 0xDFFFU)) {
        return std::nullopt;
    }
    return value;
}

// ---- the comment-stripped lines of a text -----------------------------------------------------------
// The editorSourceCodeLines rule (imgui_layer_test.cpp), copied file-locally: a citation in PROSE must
// never satisfy or break a gate about CODE.
[[nodiscard]] std::vector<std::string> codeLinesOf(std::string_view text) {
    std::vector<std::string> code;
    while (true) {
        const std::size_t newline = text.find('\n');
        const std::string_view line = newline == std::string_view::npos ? text : text.substr(0, newline);
        const std::size_t commentStart = line.find("//");
        code.emplace_back(commentStart == std::string_view::npos ? line : line.substr(0, commentStart));
        if (newline == std::string_view::npos) {
            break;
        }
        text.remove_prefix(newline + 1U);
    }
    return code;
}

[[nodiscard]] std::string readOrFail(const std::string& path) {
    const ed::FileReadResult read = ed::readTextFile(path);
    CAPTURE(path);
    REQUIRE(read.text.has_value());
    return *read.text;
}

[[nodiscard]] std::string upperSnake(std::string_view name) {
    std::string out;
    for (const char c : name) {
        out.push_back(c == '-' ? '_' : (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c);
    }
    return out;
}

[[nodiscard]] std::string hexOf(std::string_view bytes) {
    std::string out;
    for (const char c : bytes) {
        out += std::format("{:02X} ", static_cast<unsigned>(static_cast<unsigned char>(c)));
    }
    return out;
}

// ---- the literal lexer (GL6) ------------------------------------------------------------------------
// Over RAW BYTES, six states -- code, line comment, block comment, string, character, raw string --
// with a line counter. A violation is (a) a byte >= 0x80 inside a literal, (b) a \x or octal escape
// >= 0x80 outside the two macro headers, (c) a \u or \U escape anywhere, or (u) an UNTERMINATED
// literal: a newline ends a quoted literal, so a lexer that mis-reads a quote reports it rather than
// silently swallowing the text that follows.
struct Violation {
    std::size_t line = 0;
    char rule = 'a';
    std::string bytes;
};

struct LiteralScan {
    std::size_t literals = 0;
    std::size_t highEscapeLiterals = 0;
    std::vector<Violation> violations;
    std::size_t line = 1;  // the line the lexer is on -- the scan's cursor while it runs
};

// One literal's findings: at most ONE violation per rule, carrying every offending byte, so a raw
// ellipsis (three bytes) or an escaped one (three escapes) is one finding, never three.
struct LiteralFindings {
    std::array<std::optional<Violation>, 4> byRule{};
    bool highEscape = false;

    void add(char rule, std::size_t line, std::string_view bytes) {
        const std::size_t slot = rule == 'a' ? 0U : rule == 'b' ? 1U : rule == 'c' ? 2U : 3U;
        if (!byRule[slot].has_value()) {
            byRule[slot] = Violation{.line = line, .rule = rule, .bytes = std::string()};
        }
        byRule[slot]->bytes += bytes;
    }

    void flushInto(LiteralScan& scan) const {
        ++scan.literals;
        if (highEscape) {
            ++scan.highEscapeLiterals;
        }
        for (const std::optional<Violation>& found : byRule) {
            if (found.has_value()) {
                scan.violations.push_back(*found);
            }
        }
    }
};

[[nodiscard]] bool isIdentifierByte(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

[[nodiscard]] bool isDecimalDigit(char c) { return c >= '0' && c <= '9'; }

[[nodiscard]] bool isOctalDigit(char c) { return c >= '0' && c <= '7'; }

// -1 for a byte that is not a hex digit.
[[nodiscard]] int hexDigitValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// The run of identifier bytes (plus '.', when `withDot`) immediately before `at` -- a string's prefix,
// or the numeric token a quote follows.
[[nodiscard]] std::string_view runBefore(std::string_view text, std::size_t at, bool withDot) {
    std::size_t begin = at;
    while (begin > 0U && (isIdentifierByte(text[begin - 1U]) || (withDot && text[begin - 1U] == '.'))) {
        --begin;
    }
    return text.substr(begin, at - begin);
}

[[nodiscard]] bool isRawStringPrefix(std::string_view prefix) {
    return prefix == "R" || prefix == "LR" || prefix == "uR" || prefix == "UR" || prefix == "u8R";
}

// A '"' or '\'' literal opening at `open`. Returns the index just past it -- or AT the newline that ended
// an unterminated one, which the caller then counts.
std::size_t scanQuoted(std::string_view text, std::size_t open, bool allowHigh, LiteralScan& scan) {
    const char quote = text[open];
    LiteralFindings found;
    std::size_t i = open + 1U;
    bool closed = false;
    while (i < text.size()) {
        const char c = text[i];
        if (c == quote) {
            closed = true;
            ++i;
            break;
        }
        if (c == '\n') {
            break;
        }
        if (c == '\\' && i + 1U < text.size()) {
            const char escape = text[i + 1U];
            std::size_t end = i + 2U;
            unsigned value = 0;
            bool numeric = false;
            if (escape == 'x') {  // GREEDY: every hex digit that follows belongs to the escape
                numeric = true;
                while (end < text.size() && hexDigitValue(text[end]) >= 0) {
                    value = std::min(value * 16U + static_cast<unsigned>(hexDigitValue(text[end])), 0x10000U);
                    ++end;
                }
            } else if (isOctalDigit(escape)) {  // at most three octal digits
                numeric = true;
                end = i + 1U;
                while (end < text.size() && end < i + 4U && isOctalDigit(text[end])) {
                    value = (value * 8U) + static_cast<unsigned>(text[end] - '0');
                    ++end;
                }
            } else if (escape == 'u' || escape == 'U') {
                found.add('c', scan.line, text.substr(i, 2U));
            } else if (escape == '\n') {
                ++scan.line;  // a line splice inside a literal
            }
            if (numeric && value >= 0x80U) {
                if (allowHigh) {
                    found.highEscape = true;
                } else {
                    found.add('b', scan.line, text.substr(i, end - i));
                }
            }
            i = end;
            continue;
        }
        if (static_cast<unsigned char>(c) >= 0x80U) {
            found.add('a', scan.line, text.substr(i, 1U));
        }
        ++i;
    }
    if (!closed) {
        found.add('u', scan.line, text.substr(open, std::min<std::size_t>(i - open, 24U)));
    }
    found.flushInto(scan);
    return i;
}

// A raw string literal whose '"' is at `open`: R"delim( ... )delim". No escape processing inside.
std::size_t scanRaw(std::string_view text, std::size_t open, LiteralScan& scan) {
    LiteralFindings found;
    const std::size_t paren = text.find('(', open + 1U);
    const std::string_view delimiter =
        paren == std::string_view::npos ? std::string_view{} : text.substr(open + 1U, paren - open - 1U);
    if (paren == std::string_view::npos || delimiter.size() > 16U ||
        delimiter.find_first_of(" \t\n\\)") != std::string_view::npos) {
        found.add('u', scan.line, text.substr(open, 1U));
        found.flushInto(scan);
        return open + 1U;
    }
    const std::string closing = ")" + std::string(delimiter) + "\"";
    const std::size_t end = text.find(closing, paren + 1U);
    const std::size_t stop = end == std::string_view::npos ? text.size() : end;
    for (std::size_t i = paren + 1U; i < stop; ++i) {
        if (text[i] == '\n') {
            ++scan.line;
        } else if (static_cast<unsigned char>(text[i]) >= 0x80U) {
            found.add('a', scan.line, text.substr(i, 1U));
        }
    }
    if (end == std::string_view::npos) {
        found.add('u', scan.line, text.substr(open, 1U));
    }
    found.flushInto(scan);
    return end == std::string_view::npos ? text.size() : end + closing.size();
}

[[nodiscard]] LiteralScan scanLiterals(std::string_view text, bool allowHighEscapes) {
    LiteralScan scan;
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        const char next = i + 1U < text.size() ? text[i + 1U] : '\0';
        if (c == '\n') {
            ++scan.line;
            ++i;
        } else if (c == '/' && next == '/') {  // a line comment: to the newline, which the next turn counts
            const std::size_t end = text.find('\n', i);
            i = end == std::string_view::npos ? text.size() : end;
        } else if (c == '/' && next == '*') {
            const std::size_t end = text.find("*/", i + 2U);
            const std::size_t stop = end == std::string_view::npos ? text.size() : end + 2U;
            for (std::size_t j = i; j < stop; ++j) {
                scan.line += text[j] == '\n' ? 1U : 0U;
            }
            i = stop;
        } else if (c == '"') {
            if (isRawStringPrefix(runBefore(text, i, false))) {
                i = scanRaw(text, i, scan);
            } else {
                i = scanQuoted(text, i, allowHighEscapes, scan);
            }
        } else if (c == '\'') {
            // A DIGIT SEPARATOR when the token before it is numeric (1'000'000, 0xFF'FF, 1'000'000.0F) --
            // otherwise a character literal, prefixed or not (u8'x', L'x').
            const std::string_view token = runBefore(text, i, true);
            if (!token.empty() && isDecimalDigit(token.front())) {
                ++i;
            } else {
                i = scanQuoted(text, i, allowHighEscapes, scan);
            }
        } else {
            ++i;
        }
    }
    return scan;
}

[[nodiscard]] std::vector<Violation> violationsOf(const std::string& snippet, bool allowHighEscapes) {
    return scanLiterals(snippet, allowHighEscapes).violations;
}

[[nodiscard]] bool isPolicyHeader(const std::string& fileName) {
    return fileName == "editor_glyphs.hpp" || fileName == "editor_icons.hpp";
}

}  // namespace

TEST_CASE("glyphs: every glyph macro is its code point (task E.6.1, GL1)") {
    REQUIRE(ed::EDITOR_GLYPHS.size() == 13U);
    std::set<std::string_view> names;
    std::set<char32_t> codepoints;
    for (const ed::EditorGlyph& glyph : ed::EDITOR_GLYPHS) {
        CAPTURE(glyph.name);
        const std::optional<char32_t> decoded = decodeUtf8(glyph.utf8);
        REQUIRE(decoded.has_value());
        CHECK(static_cast<std::uint32_t>(*decoded) == static_cast<std::uint32_t>(glyph.codepoint));
        CHECK(glyph.utf8.size() >= 2U);
        CHECK(glyph.utf8.size() <= 3U);
        names.insert(glyph.name);
        codepoints.insert(glyph.codepoint);
    }
    CHECK(names.size() == ed::EDITOR_GLYPHS.size());
    CHECK(codepoints.size() == ed::EDITOR_GLYPHS.size());
}

TEST_CASE("glyphs: every icon macro is its code point (task E.6.1, GL2)") {
    REQUIRE(ed::EDITOR_ICONS.size() == 67U);
    std::set<std::string_view> names;
    std::set<char32_t> codepoints;
    for (const ed::EditorIcon& icon : ed::EDITOR_ICONS) {
        CAPTURE(icon.name);
        const std::optional<char32_t> decoded = decodeUtf8(icon.utf8);
        REQUIRE(decoded.has_value());
        CHECK(static_cast<std::uint32_t>(*decoded) == static_cast<std::uint32_t>(icon.codepoint));
        CHECK(icon.utf8.size() == 3U);
        CHECK(static_cast<std::uint32_t>(icon.codepoint) >= 0xE000U);  // the Private Use Area
        CHECK(static_cast<std::uint32_t>(icon.codepoint) <= 0xF8FFU);
        names.insert(icon.name);
        codepoints.insert(icon.codepoint);
    }
    CHECK(names.size() == ed::EDITOR_ICONS.size());
    CHECK(codepoints.size() == ed::EDITOR_ICONS.size());
}

TEST_CASE("glyphs: icon names are the upstream names (task E.6.1, GL3)") {
    const std::vector<std::string> code =
        codeLinesOf(readOrFail(AERO_EDITOR_INCLUDE_DIR "/aero/editor/editor_icons.hpp"));
    std::size_t defineLines = 0;
    for (const std::string& line : code) {
        defineLines += line.starts_with("#define AERO_ICON_") ? 1U : 0U;
    }
    REQUIRE(defineLines == 67U);  // anti-vacuity: the reader saw the whole roster
    for (const ed::EditorIcon& icon : ed::EDITOR_ICONS) {
        CAPTURE(icon.name);
        const std::string macro = "AERO_ICON_" + upperSnake(icon.name);
        const std::string definition = "#define " + macro + " \"";
        const auto codepoint = static_cast<std::uint32_t>(icon.codepoint);
        const std::string row = std::format("{{\"{}\", 0x{:04X}, {}}}", icon.name, codepoint, macro);
        CAPTURE(row);
        std::size_t definitions = 0;
        std::size_t rows = 0;
        for (const std::string& line : code) {
            definitions += line.starts_with(definition) ? 1U : 0U;
            rows += line.find(row) != std::string::npos ? 1U : 0U;
        }
        CHECK(definitions == 1U);
        CHECK(rows == 1U);
    }
}

TEST_CASE("glyphs: the roster is upstream's codepoints.json (task E.6.1, GL4)") {
    // A name in the JSON is NOT proof the font draws it -- I278 is that proof. This pins the NAMES and the
    // code points against the vendored upstream table, so a Lucide release that moves one is red here.
    const std::string text = readOrFail(AERO_EDITOR_FONTS_DIR "/lucide/codepoints.json");
    const engine::JsonParseResult parsed = engine::parseJson(text);
    CAPTURE(parsed.error.message);
    REQUIRE(parsed.ok());
    const engine::JsonValue& root = *parsed.value;
    REQUIRE(root.isObject());
    CHECK(root.size() > 2000U);  // anti-vacuity: 2141 names measured
    for (const ed::EditorIcon& icon : ed::EDITOR_ICONS) {
        CAPTURE(icon.name);
        const engine::JsonValue* entry = root.find(icon.name);
        REQUIRE(entry != nullptr);
        const std::optional<std::uint64_t> value = entry->asU64();
        REQUIRE(value.has_value());
        CHECK(*value == static_cast<std::uint64_t>(icon.codepoint));
    }
}

TEST_CASE("glyphs: no glyph Plex lacks (task E.6.1, GL5)") {
    // spec D19.4: Plex draws none of these -- the command, shift, option, control, erase, return and
    // enter symbols, and the two small triangles -- so none may become a glyph macro.
    constexpr std::array<std::uint32_t, 9> ABSENT{0x2318U, 0x21E7U, 0x2325U, 0x2303U, 0x232BU,
                                                  0x23CEU, 0x21B5U, 0x25B8U, 0x25BEU};
    for (const std::uint32_t absent : ABSENT) {
        CAPTURE(absent);
        for (const ed::EditorGlyph& glyph : ed::EDITOR_GLYPHS) {
            CHECK(static_cast<std::uint32_t>(glyph.codepoint) != absent);
        }
    }
}

TEST_CASE("glyphs: no editor literal carries a non-ASCII byte (task E.6.1, GL6)") {
    // ---- the lexer's own self-tests, first: a broken lexer must not pass by seeing nothing ----
    const std::string rawInString = "s = \"" + ellipsisBytes + "\";";
    {
        const std::vector<Violation> v = violationsOf(rawInString, false);
        REQUIRE(v.size() == 1U);
        CHECK(v[0].rule == 'a');
        CHECK(v[0].line == 1U);
    }
    CHECK(violationsOf("// " + ellipsisBytes, false).empty());
    CHECK(violationsOf("/* " + ellipsisBytes + " */", false).empty());
    CHECK(violationsOf("s = R\"x(" + ellipsisBytes + ")x\";", false).size() == 1U);
    {
        const std::vector<Violation> v = violationsOf(R"(c = '\xE2';)", false);
        REQUIRE(v.size() == 1U);
        CHECK(v[0].rule == 'b');
    }
    {
        const std::string escaped = R"(s = "\xE2\x80\xA6";)";
        const std::vector<Violation> refused = violationsOf(escaped, false);
        REQUIRE(refused.size() == 1U);
        CHECK(refused[0].rule == 'b');
        const LiteralScan allowed = scanLiterals(escaped, true);
        CHECK(allowed.violations.empty());
        CHECK(allowed.highEscapeLiterals == 1U);
    }
    {
        // The six-character universal-character-name escape. "\\u" below is an escaped BACKSLASH and a "u",
        // so the snippet holds the six characters on every compiler -- a raw string is avoided on purpose,
        // since a compiler that processed the escape inside one would hand the lexer a raw byte instead.
        // NOLINTNEXTLINE(modernize-raw-string-literal)
        const std::vector<Violation> v = violationsOf("s = \"\\u00E9\";", false);
        REQUIRE(v.size() == 1U);
        CHECK(v[0].rule == 'c');
    }
    {
        // A digit separator opens nothing, so the next line's literal is still found, on the right line.
        const std::vector<Violation> v = violationsOf("x = 1'000'000;\n" + rawInString, false);
        REQUIRE(v.size() == 1U);
        CHECK(v[0].line == 2U);
    }
    CHECK(violationsOf("c = '\"';\n" + rawInString, false).size() == 1U);  // a quote CHARACTER opens nothing
    // asset_actions.cpp's shape: a raw string holding a quote ends at )" and nowhere earlier.
    const std::string rawWithQuote = R"x(bad = R"(*?"<>|:)";)x";
    CHECK(violationsOf(rawWithQuote + "\n" + rawInString, false).size() == 1U);
    CHECK(violationsOf("#include <foo/bar.h>", false).empty());

    // ---- then the tree ----
    std::vector<std::pair<std::string, std::string>> files;  // (display name, absolute path)
    const auto collect = [&files](const std::string& directory, std::string_view prefix, bool takeCpp) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
            if (!entry.is_regular_file(ec)) {
                continue;
            }
            const std::string name = entry.path().filename().string();
            if (name.ends_with(".hpp") || (takeCpp && name.ends_with(".cpp"))) {
                files.emplace_back(std::string(prefix) + name, entry.path().string());
            }
        }
        CAPTURE(directory);
        REQUIRE_FALSE(ec);
    };
    collect(AERO_EDITOR_SRC_DIR, "editor/src/", true);
    collect(AERO_EDITOR_INCLUDE_DIR "/aero/editor", "editor/include/aero/editor/", false);
    std::sort(files.begin(), files.end());

    std::size_t literals = 0;
    std::size_t totalViolations = 0;
    std::size_t highEscapesInTheTwoHeaders = 0;
    std::size_t policyHeaders = 0;
    for (const auto& [file, path] : files) {
        const std::string fileName = std::filesystem::path(path).filename().string();
        const bool policyHeader = isPolicyHeader(fileName) && file.starts_with("editor/include/");
        const LiteralScan scan = scanLiterals(readOrFail(path), policyHeader);
        literals += scan.literals;
        if (policyHeader) {
            ++policyHeaders;
            highEscapesInTheTwoHeaders += scan.highEscapeLiterals;
        }
        for (const Violation& violation : scan.violations) {
            CAPTURE(file);
            CAPTURE(violation.line);
            CAPTURE(violation.rule);
            const std::string bytes = hexOf(violation.bytes);
            CAPTURE(bytes);
            FAIL_CHECK("a literal breaks the 7-bit policy (.claude/rules/editor.md)");
            ++totalViolations;
        }
    }
    MESSAGE("GL6 scanned ", files.size(), " files and ", literals, " literals");
    CHECK(totalViolations == 0U);
    CHECK(policyHeaders == 2U);
    // The scan SEES the escapes it permits: one high-escape literal per macro, and none elsewhere in the
    // two headers.
    CHECK(highEscapesInTheTwoHeaders == ed::EDITOR_GLYPHS.size() + ed::EDITOR_ICONS.size());
    // Anti-vacuity floors, measured when this case landed on top of 750f303: 195 files and 2615 literals.
    // The literal floor is that count less a twentieth (2615 - 2615 / 20), so ordinary growth or a small
    // deletion passes and a scan that silently stopped seeing most literals does not.
    CHECK(files.size() > 180U);
    CHECK(literals > 2485U);
}

TEST_CASE("glyphs: the elision constants are the ellipsis (task E.6.1, GL7)") {
    CHECK(ed::MATERIAL_DISPLAY_NAME_ELLIPSIS == std::string_view{AERO_GLYPH_ELLIPSIS});
    CHECK(ed::MATERIAL_DISPLAY_NAME_ELLIPSIS.size() == 3U);  // the elision arithmetic depends on the length

    // asset_view.cpp's file-local CAPTION_ELLIPSIS, through its EFFECT: a cut that fits nothing longer
    // than five bytes keeps two bytes and the three-byte ellipsis.
    const ed::CaptionLineFits fitsFiveBytes = [](std::string_view text) { return text.size() <= 5U; };
    const std::string elided = ed::elideCaptionRight("abcdefghij", fitsFiveBytes);
    REQUIRE(elided.size() >= 3U);
    CHECK(std::string_view(elided).substr(elided.size() - 3U) == std::string_view{AERO_GLYPH_ELLIPSIS});
    CHECK(elided == std::string("ab") + AERO_GLYPH_ELLIPSIS);

    // ...and its spelling, as source text.
    const std::vector<std::string> code = codeLinesOf(readOrFail(AERO_EDITOR_SRC_DIR "/asset_view.cpp"));
    std::size_t spelled = 0;
    for (const std::string& line : code) {
        spelled += line.find("CAPTION_ELLIPSIS = AERO_GLYPH_ELLIPSIS;") != std::string::npos ? 1U : 0U;
    }
    CHECK(spelled == 1U);
}
