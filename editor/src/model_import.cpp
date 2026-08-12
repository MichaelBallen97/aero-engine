// Aero Engine — model_import.cpp: the pure dispatch and URI-policy half of the model importer (task
// 3.2.1). fastgltf-free AT SOURCE: this TU includes only "gltf_import.hpp" (the src-private
// DECLARATION) and calls importGltf(); the glTF backend itself lives entirely in gltf_import.cpp.
// NOTHING HERE LOGS (INV-A3), NOTHING HERE TOUCHES DISK (INV-M3), NOTHING HERE THROWS.
#include <aero/editor/model_import.hpp>

#include "assimp_import.hpp"
#include "fbx_import.hpp"
#include "gltf_import.hpp"
#include "obj_import.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::editor {

namespace {

// ASCII-only, locale-independent. NEVER std::tolower(char): UTF-8 continuation bytes are NEGATIVE as
// char, which is UB and trips bugprone-signed-char-misuse (asset_meta.cpp/project_files.cpp's
// precedent -- this file keeps its OWN copy rather than sharing one, matching that precedent).
constexpr unsigned char foldAscii(unsigned char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + ('a' - 'A')) : c;
}

// The suffix test isImportableModelName has always performed, LIFTED OUT OF ITS LOOP so the DISPATCH
// and the IDENTITY can use the identical comparison. Behaviour unchanged: ASCII case fold, and the
// isMetaFileName shape -- ".fbx" alone is not a model, something must precede the extension.
// ONE comparison, THREE callers -- which is the shape 3.2.1's own code review asked for when it
// rejected a TU-local copy in asset_cache.cpp.
[[nodiscard]] bool endsWithFolded(std::string_view name, std::string_view ext) noexcept {
    if (name.size() <= ext.size()) {
        return false;  // the isMetaFileName shape: ".gltf" alone needs something BEFORE the extension
    }
    const std::size_t offset = name.size() - ext.size();
    for (std::size_t i = 0; i < ext.size(); ++i) {
        if (foldAscii(static_cast<unsigned char>(name[offset + i])) != foldAscii(static_cast<unsigned char>(ext[i]))) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::string_view importStatusLabel(ImportStatus status) noexcept {
    switch (status) {
        case ImportStatus::Ok:
            return "Ok";
        case ImportStatus::Unsupported:
            return "Unsupported";
        case ImportStatus::ParseFailed:
            return "Parse failed";
        case ImportStatus::Malformed:
            return "Malformed";
        case ImportStatus::MissingExtension:
            return "Missing extension";
        case ImportStatus::MissingBuffer:
            return "Missing buffer";
        case ImportStatus::Truncated:
            return "Truncated";
    }
    return "Unknown";
}

bool isImportableModelName(std::string_view fileName) noexcept {
    constexpr std::array<std::string_view, 8> EXTENSIONS = {".gltf", ".glb", ".fbx", ".obj",
                                                            ".mtl",  ".dae", ".ply", ".stl"};
    for (const std::string_view ext : EXTENSIONS) {
        if (endsWithFolded(fileName, ext)) {
            return true;
        }
    }
    return false;
}

ImporterIdentity modelImporterIdentity(std::string_view fileName) noexcept {
    if (endsWithFolded(fileName, ".fbx")) {
        return {FBX_IMPORTER_NAME, FBX_IMPORTER_VERSION};
    }
    // task 3.2.3: ONE identity for BOTH claimed extensions -- one importer, two file kinds. A .mtl's
    // cache entry therefore records ("obj", 1), which is what makes an OBJ_IMPORTER_VERSION bump
    // re-trigger imports for .obj AND .mtl together and for nothing else.
    if (endsWithFolded(fileName, ".obj") || endsWithFolded(fileName, ".mtl")) {
        return {OBJ_IMPORTER_NAME, OBJ_IMPORTER_VERSION};
    }
    // task 3.2.5: ONE identity for ALL THREE claimed extensions -- one importer, three file kinds. A
    // .stl's cache entry therefore records ("assimp", 1), which is what makes an
    // ASSIMP_IMPORTER_VERSION bump re-trigger imports for .dae, .ply and .stl together and for
    // nothing else.
    if (endsWithFolded(fileName, ".dae") || endsWithFolded(fileName, ".ply") || endsWithFolded(fileName, ".stl")) {
        return {ASSIMP_IMPORTER_NAME, ASSIMP_IMPORTER_VERSION};
    }
    if (isImportableModelName(fileName)) {
        return {GLTF_IMPORTER_NAME, GLTF_IMPORTER_VERSION};
    }
    return {};  // ("", 0) -- exactly ImportInput's own un-probed defaults, so nothing about a
                // non-model asset's plan changes
}

bool modelImporterNeedsExternalBuffers(std::string_view fileName) noexcept {
    // FBX: NO -- all geometry is in the file, and its external URIs are TEXTURES.
    if (endsWithFolded(fileName, ".fbx")) {
        return false;
    }
    // .mtl: NO -- a material library's whole content is LOCAL (D6). Its external URIs are TEXTURES,
    // which this importer resolves for the DEPENDENCY GRAPH and never reads. Answering TRUE here would
    // make ModelImportSession read every texture the library names, hand them to an arm that ignores
    // them, and -- once they exceed MAX_EXTERNAL_BYTES_PER_MODEL -- report Truncated for a result that
    // was complete at Structure depth.
    if (endsWithFolded(fileName, ".mtl")) {
        return false;
    }
    // task 3.2.5: NO, for all three. Every external reference .dae/.ply/.stl carry is a TEXTURE, which
    // this importer resolves for the DEPENDENCY GRAPH and never reads -- the FBX answer verbatim. That
    // is precisely why ModelImportSession needed no edit for this task: service() skips its whole first
    // pass and runs ONE Full import with an empty external span, a path FBX already ships and validates.
    if (endsWithFolded(fileName, ".dae") || endsWithFolded(fileName, ".ply") || endsWithFolded(fileName, ".stl")) {
        return false;
    }
    // .gltf/.glb (buffers may be external .bin files) and .obj (its .mtl IS an external file, D4).
    return isImportableModelName(fileName);
}

std::string foldBackslashesToSlashes(std::string_view path) {
    std::string out(path);
    for (char& c : out) {
        if (c == '\\') {
            c = '/';
        }
    }
    return out;
}

namespace {

// task 3.2.5 (A-10). PURE, LOCALE-FREE decimal -> float, fully consumed or refused.
//
// MEASURED DEVIATION FROM THE PLAN, and the reason is a toolchain fact rather than a preference:
// std::from_chars's FLOATING-POINT overload does not exist on every toolchain this project builds
// with. Apple's libc++ 19.1.2 (MacOSX15.4.sdk -- the SDK this repo's own clang-tidy invocation pins,
// and the one an Xcode 16 CI runner supplies) and Homebrew LLVM 18's own libc++ both ship
// __charconv/from_chars_integral.h ONLY, so `std::from_chars(first, last, float&)` is a hard
// "call to deleted function" there. engine/reflect/src/json_value.cpp already discovered this and
// guards its use behind __cpp_lib_to_chars with a strtof_l fallback -- but that fallback needs
// per-OS includes, and editor/src carries exactly three per-OS lines in exactly one file
// (blender_tool.cpp) and must keep it that way. So this reads the number itself.
//
// std::stof/std::atof stay refused for the reason they always were: stof reads the C locale's
// decimal point, so a German-locale editor would read "0.01" as 0, and it throws. This does neither.
// Grammar: [+-]? DIGITS [ '.' DIGITS ] [ (e|E) [+-] DIGITS ], with FULL CONSUMPTION -- "0.01abc" is
// refused whole, which is the property MI148 pins. Rounding is within a fraction of a ULP of
// correctly-rounded, which is irrelevant here: the value is DISPLAY-ONLY (A-10) and is never fed
// back into geometry, compared or switched on.
[[nodiscard]] std::optional<float> parseDecimalFloat(std::string_view text) noexcept {
    std::size_t i = 0;
    bool negative = false;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
        negative = text[i] == '-';
        ++i;
    }
    double mantissa = 0.0;
    int exponent = 0;
    std::size_t digits = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
        mantissa = (mantissa * 10.0) + static_cast<double>(text[i] - '0');
        ++digits;
        ++i;
    }
    if (i < text.size() && text[i] == '.') {
        ++i;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            mantissa = (mantissa * 10.0) + static_cast<double>(text[i] - '0');
            --exponent;
            ++digits;
            ++i;
        }
    }
    if (digits == 0) {
        return std::nullopt;  // "", ".", "nan", "inf", "+" -- none of them is a number here
    }
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        bool exponentNegative = false;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            exponentNegative = text[i] == '-';
            ++i;
        }
        std::size_t exponentDigits = 0;
        int magnitude = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            if (magnitude < 10000) {  // saturate rather than overflow; 1e10000 is refused below anyway
                magnitude = (magnitude * 10) + (text[i] - '0');
            }
            ++exponentDigits;
            ++i;
        }
        if (exponentDigits == 0) {
            return std::nullopt;
        }
        exponent += exponentNegative ? -magnitude : magnitude;
    }
    if (i != text.size()) {
        return std::nullopt;  // FULL CONSUMPTION: trailing garbage refuses the whole value
    }
    const double scaled = mantissa * std::pow(10.0, static_cast<double>(exponent));
    const double value = negative ? -scaled : scaled;
    // The magnitude test is BEFORE the narrowing cast, not after: converting a double larger than
    // FLT_MAX to float is undefined behaviour, and the Debug lanes run UBSan.
    if (!std::isfinite(value) || std::abs(value) > static_cast<double>(std::numeric_limits<float>::max())) {
        return std::nullopt;
    }
    return static_cast<float>(value);
}

// task 3.2.3: the ONE place that decides "is this line an mtllib DIRECTIVE, and what's its operand" --
// scanObjMtlLibs's own inner loop is the only caller. Kept as a free function rather than inlined so
// its rule (case-SENSITIVE, leading-whitespace-only, a space or tab separator) is stated once.
[[nodiscard]] bool mtllibOperand(std::string_view line, std::string_view& operandOut) {
    constexpr std::string_view KEYWORD = "mtllib";
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    if (line.size() <= i + KEYWORD.size() || line.substr(i, KEYWORD.size()) != KEYWORD) {
        return false;
    }
    if (line[i + KEYWORD.size()] != ' ' && line[i + KEYWORD.size()] != '\t') {
        return false;
    }
    const std::string_view operand = line.substr(i + KEYWORD.size());
    std::size_t start = 0;
    while (start < operand.size() && (operand[start] == ' ' || operand[start] == '\t')) {
        ++start;
    }
    std::size_t end = operand.size();
    while (end > start && (operand[end - 1] == ' ' || operand[end - 1] == '\t')) {
        --end;
    }
    operandOut = operand.substr(start, end - start);
    return true;
}

}  // namespace

ObjMtlLibScan scanObjMtlLibsScan(std::span<const std::byte> bytes, std::size_t maxNames) {
    ObjMtlLibScan scan;
    if (bytes.empty()) {
        return scan;
    }
    // NOTE: `maxNames == 0` is NOT an early return here, unlike scanObjMtlLibs's own former standalone
    // shape -- emptyOperandLines is orthogonal to the candidate cap (an empty operand has no candidate
    // to cap in the first place), so the scan still runs and still counts E4 correctly; `pushCandidate`'s
    // own `>= maxNames` check (0 >= 0) already keeps `candidates` empty on its own, matching the old
    // behaviour for that half exactly.
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());

    const auto pushCandidate = [&scan, maxNames](std::string_view candidate) {
        if (scan.candidates.size() >= maxNames) {  // cap BEFORE the push (INV-O10)
            return;
        }
        for (const std::string& existing : scan.candidates) {  // dedup BY RAW TEXT, order preserved
            if (existing == candidate) {
                return;
            }
        }
        scan.candidates.emplace_back(candidate);
    };

    std::size_t lineStart = 0;
    while (lineStart <= text.size()) {
        const std::size_t newline = text.find('\n', lineStart);
        std::string_view line =
            newline == std::string_view::npos ? text.substr(lineStart) : text.substr(lineStart, newline - lineStart);
        if (!line.empty() && line.back() == '\r') {  // ONE trailing '\r' (CRLF) -- E7
            line.remove_suffix(1);
        }

        std::string_view operand;
        if (mtllibOperand(line, operand)) {
            if (operand.empty()) {
                // code-review round, gap 10: E4, counted in THIS SAME PASS -- the caller-side
                // countEmptyMtllibOperandLines used to re-walk the whole file a second time purely to
                // recover this count; the two scans were previously identical in shape and always ran
                // together, so folding it in here removes a duplicate ~150 MB linear scan from every
                // Structure probe.
                ++scan.emptyOperandLines;
            } else {
                pushCandidate(operand);  // the WHOLE operand LEADS
                std::size_t tokenStart = 0;
                while (tokenStart < operand.size()) {
                    while (tokenStart < operand.size() && (operand[tokenStart] == ' ' || operand[tokenStart] == '\t')) {
                        ++tokenStart;
                    }
                    std::size_t tokenEnd = tokenStart;
                    while (tokenEnd < operand.size() && operand[tokenEnd] != ' ' && operand[tokenEnd] != '\t') {
                        ++tokenEnd;
                    }
                    if (tokenEnd > tokenStart) {
                        pushCandidate(operand.substr(tokenStart, tokenEnd - tokenStart));
                    }
                    tokenStart = tokenEnd;
                }
            }
        }

        if (newline == std::string_view::npos) {
            break;
        }
        lineStart = newline + 1;
    }
    return scan;
}

std::vector<std::string> scanObjMtlLibs(std::span<const std::byte> bytes, std::size_t maxNames) {
    return scanObjMtlLibsScan(bytes, maxNames).candidates;
}

// task 3.2.5 (A-7/A-19b). See the header for the contract and for WHY the rule is the library's own
// rather than a corrected one.
std::vector<std::string> scanPlyTextureFiles(std::span<const std::byte> bytes, std::size_t maxNames) {
    std::vector<std::string> out;
    if (bytes.empty()) {
        return out;
    }
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());

    constexpr std::string_view SEMANTIC = "TextureFile";
    std::size_t lineStart = 0;
    while (lineStart <= text.size()) {
        const std::size_t newline = text.find('\n', lineStart);
        std::string_view line =
            newline == std::string_view::npos ? text.substr(lineStart) : text.substr(lineStart, newline - lineStart);
        if (!line.empty() && line.back() == '\r') {  // ONE trailing '\r' (CRLF) -- E7, scanObjMtlLibsScan's rule
            line.remove_suffix(1);
        }

        // The library's own element-line test: after LEADING WHITESPACE, the line must begin with
        // `element` or `comment`. Anything else is not a candidate at all.
        //
        // WHITESPACE IS SPACE **AND TAB**, because that is what the library means by it: PLY::Element
        // ::ParseElement opens with PLY::DOM::SkipSpaces, which forwards to Assimp::SkipSpaces, whose
        // test is `(in == ' ' || in == '\t')` (the port's own ParsingUtils.h). Skipping only ' ' here
        // made a TAB-indented `comment TextureFile wood.png` invisible to this scan and visible to the
        // loader -- Structure returning {} where Full returns {wood.png}, the one disagreement AC-19
        // forbids, and a dependency phase 7.5 would never record. The identical rule is applied in
        // plyDeclaredCountsExceedBytes below; the two must never diverge.
        std::size_t i = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
            ++i;
        }
        const std::string_view rest = line.substr(i);
        if (rest.substr(0, 7) == "element" || rest.substr(0, 7) == "comment") {
            // Skip the keyword, then the whitespace after it, then match the CASE-SENSITIVE semantic.
            std::size_t j = 7;
            while (j < rest.size() && (rest[j] == ' ' || rest[j] == '\t')) {
                ++j;
            }
            if (rest.substr(j, SEMANTIC.size()) == SEMANTIC) {
                std::size_t k = j + SEMANTIC.size();
                while (k < rest.size() && (rest[k] == ' ' || rest[k] == '\t')) {
                    ++k;
                }
                // THE OPERAND IS THE REST OF THE LINE, VERBATIM -- INCLUDING ANY TRAILING SPACE.
                //
                // MEASURED against assimp 6.0.4, correcting the plan's own prediction. The library
                // takes `std::string(&buffer[0], &buffer[0] + strlen(&buffer[0]) - 1)`, which LOOKS
                // like an off-by-one that eats the last character -- but the buffer it reads still
                // carries the line terminator, so the -1 removes exactly that and nothing else.
                // Confirmed on four fixtures: `comment TextureFile a.png ` yields `"a.png "` WITH the
                // space, `comment TextureFile a.png` yields `"a.png"`, and CRLF behaves as LF does.
                //
                // So the trailing space SURVIVES, and this scan must let it survive too. Trimming it
                // would make Structure report `a.png` while Full's own material reports `a.png `, the
                // two would classify to different relative paths, and the depths would DISAGREE about
                // the URI set -- which is the one thing AC-19 forbids. Do not "fix" this.
                //
                // The one shape the two still differ on is a `TextureFile` line that is the file's
                // LAST line with no terminator at all, where the library's -1 does eat a real
                // character. A valid .ply cannot produce it: `end_header` always follows.
                const std::string_view operand = rest.substr(k);
                if (!operand.empty()) {
                    bool seen = false;
                    for (const std::string& existing : out) {  // dedup BY RAW TEXT, order preserved
                        if (existing == operand) {
                            seen = true;
                            break;
                        }
                    }
                    if (!seen && out.size() < maxNames) {  // cap BEFORE the push
                        out.emplace_back(operand);
                    }
                }
            }
        }
        if (rest.substr(0, 10) == "end_header") {
            break;  // BOUNDED BY THE HEADER: nothing past this line is scanned, at any file size
        }

        if (newline == std::string_view::npos) {
            break;
        }
        lineStart = newline + 1;
    }
    return out;
}

// task 3.2.5 (R8). See the header for the contract and for the measurement that forced this to exist.
bool plyDeclaredCountsExceedBytes(std::span<const std::byte> bytes) {
    if (bytes.empty()) {
        return false;  // an empty file is a PARSE failure, not a lying one -- let the loader say so
    }
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());

    // Saturating, because the operand is user-controlled text: `element vertex 99999999999999999999`
    // must not wrap into a small number and pass the check it is meant to fail.
    unsigned long long declared = 0;
    std::size_t lineStart = 0;
    while (lineStart <= text.size()) {
        const std::size_t newline = text.find('\n', lineStart);
        std::string_view line =
            newline == std::string_view::npos ? text.substr(lineStart) : text.substr(lineStart, newline - lineStart);
        if (!line.empty() && line.back() == '\r') {  // ONE trailing '\r' -- scanPlyTextureFiles' rule
            line.remove_suffix(1);
        }
        // SPACE AND TAB, identical to scanPlyTextureFiles' rule above and to the library's own
        // Assimp::SkipSpaces. Skipping only ' ' here failed SAFE -- a tab-indented `element` line was
        // simply not counted, so the check under-counted and could never reject an honest file -- but
        // the two scans read the same header and a reader who checks one must find the other agrees.
        std::size_t i = 0;
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
            ++i;
        }
        const std::string_view rest = line.substr(i);

        if (rest.substr(0, 10) == "end_header") {
            // The ONLY place this function returns a verdict: everything the body must hold is known,
            // and so is how many bytes there are to hold it.
            const std::size_t bodyStart = newline == std::string_view::npos ? text.size() : newline + 1;
            return declared > static_cast<unsigned long long>(text.size() - bodyStart);
        }

        // `element <name> <count>`, the keyword followed by whitespace so `elementary` cannot match.
        if (rest.substr(0, 7) == "element" && rest.size() > 7 && (rest[7] == ' ' || rest[7] == '\t')) {
            std::size_t j = 7;
            for (int field = 0; field < 2; ++field) {  // skip the spaces, then the <name> token
                while (j < rest.size() && (rest[j] == ' ' || rest[j] == '\t')) {
                    ++j;
                }
                if (field == 0) {
                    while (j < rest.size() && rest[j] != ' ' && rest[j] != '\t') {
                        ++j;
                    }
                }
            }
            unsigned long long count = 0;
            bool anyDigit = false;
            while (j < rest.size() && rest[j] >= '0' && rest[j] <= '9') {
                anyDigit = true;
                const auto digit = static_cast<unsigned long long>(rest[j] - '0');
                constexpr unsigned long long LIMIT = std::numeric_limits<unsigned long long>::max();
                count = (count > (LIMIT - digit) / 10ULL) ? LIMIT : (count * 10ULL) + digit;
                ++j;
            }
            if (anyDigit) {
                declared = (declared > std::numeric_limits<unsigned long long>::max() - count)
                               ? std::numeric_limits<unsigned long long>::max()
                               : declared + count;
            }
        }

        if (newline == std::string_view::npos) {
            break;
        }
        lineStart = newline + 1;
    }
    // No `end_header` at all, so this is not a well-formed header and there is no body to compare
    // against. Refusing here would reject a truncated-but-honest file; the loader reports it instead.
    return false;
}

// task 3.2.5 (A-10). See the header for the contract. DISPLAY-ONLY: nothing here is fed back into
// geometry, compared, or switched on.
//
// A TEXT scan rather than an XML parse, deliberately: pugixml arrives transitively with assimp, and
// using it here was considered and rejected -- it would be a second source of truth about the same
// document, it builds a DOM whose size is a multiple of the file's, and the moment it disagreed with
// Assimp about anything there would be no correct side.
SourceSpace scanColladaAssetSpace(std::span<const std::byte> bytes, std::size_t maxBytes) {
    SourceSpace out;  // declared == false, unitMeters == 1, upAxis == 'Y' -- the "found nothing" answer
    if (bytes.empty()) {
        return out;
    }
    const std::size_t n = std::min(bytes.size(), maxBytes);
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), n);

    // <unit meter="0.01" name="centimeter"/> -- the ATTRIBUTE is what matters; `name` is prose we do
    // not read. Located by the literal `meter="` inside the first `<unit` element, so an attribute
    // order of (name, meter) works as well as (meter, name).
    const std::size_t unitTag = text.find("<unit");
    if (unitTag != std::string_view::npos) {
        const std::size_t close = text.find('>', unitTag);
        const std::string_view element =
            text.substr(unitTag, close == std::string_view::npos ? std::string_view::npos : close - unitTag);
        const std::size_t attr = element.find("meter=\"");
        if (attr != std::string_view::npos) {
            const std::string_view valueStart = element.substr(attr + 7);
            const std::size_t quote = valueStart.find('"');
            if (quote != std::string_view::npos) {
                // parseDecimalFloat is LOCALE-INDEPENDENT, never throws, and refuses trailing
                // garbage whole -- see its own comment for why std::from_chars is not usable here
                // and why std::stof never was. A non-finite or non-positive unit is refused: it is
                // not a unit, and inventing one is what this task's scoping rejected.
                const std::optional<float> meters = parseDecimalFloat(valueStart.substr(0, quote));
                if (meters.has_value() && *meters > 0.0F) {
                    out.unitMeters = *meters;
                    out.declared = true;
                }
            }
        }
    }

    // <up_axis>Z_UP</up_axis> -- the first character of the element's text is the axis letter.
    const std::size_t upTag = text.find("<up_axis>");
    if (upTag != std::string_view::npos) {
        const std::string_view value = text.substr(upTag + 9);
        std::size_t v = 0;
        while (v < value.size() && (value[v] == ' ' || value[v] == '\t' || value[v] == '\r' || value[v] == '\n')) {
            ++v;
        }
        if (v < value.size()) {
            const unsigned char axis = foldAscii(static_cast<unsigned char>(value[v]));
            if (axis == 'x' || axis == 'y' || axis == 'z') {
                out.upAxis = static_cast<char>(axis - ('a' - 'A'));  // 'X' | 'Y' | 'Z', SourceSpace's own domain
                out.declared = true;
            }
        }
    }
    return out;
}

bool looksLikeBinaryContent(std::span<const std::byte> bytes, std::size_t probeBytes) noexcept {
    const std::size_t n = std::min(bytes.size(), probeBytes);
    for (std::size_t i = 0; i < n; ++i) {
        if (bytes[i] == std::byte{0}) {
            return true;
        }
    }
    return false;
}

std::optional<std::string> normalizeRelativePath(std::string_view path) {
    std::vector<std::string_view> stack;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::string_view segment =
            slash == std::string_view::npos ? path.substr(start) : path.substr(start, slash - start);
        if (segment.empty() || segment == ".") {
            // dropped -- an empty segment (from "//" or a trailing '/') or a "." segment contributes nothing
        } else if (segment == "..") {
            if (stack.empty()) {
                return std::nullopt;  // underflow: the path escapes its root
            }
            stack.pop_back();
        } else {
            stack.push_back(segment);
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    std::string joined;
    for (std::size_t i = 0; i < stack.size(); ++i) {
        if (i != 0) {
            joined += '/';
        }
        joined += stack[i];
    }
    return joined;
}

UriClassification classifyUri(std::string_view uri, std::string_view assetRelativeDir) {
    UriClassification out;

    // 1. Control characters FIRST (plan A1). fastgltf's decodePercents turns "%zz" into a NUL byte
    // (strtoul("zz", nullptr, 16) == 0), and a path with an embedded NUL or newline is never legitimate.
    for (const char c : uri) {
        if (static_cast<unsigned char>(c) < 0x20U) {
            out.kind = UriClass::RefusedControlChar;
            out.reason = "the URI contains a control character";
            return out;
        }
    }
    if (uri.empty()) {
        out.kind = UriClass::RefusedEmpty;
        out.reason = "the URI is empty";
        return out;
    }

    // 2. Backslash -- a STRING test, so macOS and Windows refuse identically (E26).
    if (uri.find('\\') != std::string_view::npos) {
        out.kind = UriClass::RefusedBackslash;
        out.reason = "the URI contains a backslash; a glTF URI separator is '/'";
        return out;
    }

    // 3. Scheme. A ':' anywhere before the first '/' is a scheme (RFC 3986), which also catches a
    // Windows drive letter "C:/x" -- deliberately, since that is an absolute path either way.
    const std::size_t firstSlash = uri.find('/');
    const std::size_t firstColon = uri.find(':');
    if (firstColon != std::string_view::npos && (firstSlash == std::string_view::npos || firstColon < firstSlash)) {
        const std::string_view scheme = uri.substr(0, firstColon);
        if (scheme == "data") {
            out.kind = UriClass::DataUri;  // decoded by fastgltf itself; embedded; never a dependency
            return out;
        }
        out.kind = UriClass::RefusedScheme;
        out.reason =
            "refused: the URI names a scheme ('" + std::string(scheme) + "'), which this importer never follows";
        return out;
    }

    // 4. Absolute paths.
    if (uri.front() == '/') {
        out.kind = UriClass::RefusedAbsolute;
        out.reason = "refused: the URI is an absolute path";
        return out;
    }

    // 5. Resolve against the model's own directory, then normalize. `..` segments are ALLOWED and
    // resolved (D14) -- models/chair.gltf -> ../textures/wood.png is ordinary authoring. What is
    // refused is the RESULT landing outside the assets root.
    std::string joined;
    if (!assetRelativeDir.empty()) {
        joined.assign(assetRelativeDir);
        joined += '/';
    }
    joined += uri;
    const std::optional<std::string> normalized = normalizeRelativePath(joined);
    if (!normalized.has_value()) {
        out.kind = UriClass::RefusedEscape;
        out.reason = "refused: the URI resolves outside the project's assets folder";
        return out;
    }
    if (normalized->empty()) {
        out.kind = UriClass::RefusedEmpty;
        out.reason = "the URI resolves to nothing";
        return out;
    }
    out.kind = UriClass::RelativePath;
    out.relativePath = *normalized;
    return out;
}

ImportResult importModel(std::string_view fileName, std::string_view assetRelativeDir, std::span<const std::byte> bytes,
                         const ImportSettings& settings, ImportDepth depth, std::span<const ExternalBuffer> external) {
    // The FBX and OBJ arms FIRST, so the glTF arm below stays the "everything else importable" case and
    // no two arms ever claim a name. MI105/MI105b/MI105c keep the suffix table, the identity table and
    // this chain in sync -- a fourth importer added to one but not the others is a RED case, not a
    // silent misroute.
    //
    // STILL AN IF-CHAIN AT THREE ARMS, DELIBERATELY (task 3.2.3, §A-8): a dispatch table needs a
    // UNIFORM backend signature, and importObj must additionally take `fileName` because it has two
    // arms. Unifying would mean editing gltf_import.hpp, whose byte-identity to `main` this task pays
    // to keep. The table that matters already exists on the test side, in MI105's own array.
    if (endsWithFolded(fileName, ".fbx")) {
        return importFbx(assetRelativeDir, bytes, settings, depth, external);
    }
    if (endsWithFolded(fileName, ".obj") || endsWithFolded(fileName, ".mtl")) {
        return importObj(fileName, assetRelativeDir, bytes, settings, depth, external);
    }
    // task 3.2.5: THREE claimed extensions, ONE backend, and `fileName` is what selects the arm inside
    // it -- the .obj/.mtl shape one step wider. Placed BEFORE the glTF arm for the same reason the FBX
    // and OBJ arms are: the glTF arm stays the "everything else importable" case, so no two arms ever
    // claim a name.
    if (endsWithFolded(fileName, ".dae") || endsWithFolded(fileName, ".ply") || endsWithFolded(fileName, ".stl")) {
        return importAssimp(fileName, assetRelativeDir, bytes, settings, depth, external);
    }
    if (isImportableModelName(fileName)) {  // .gltf / .glb
        return importGltf(assetRelativeDir, bytes, settings, depth, external);
    }
    ImportResult result;
    result.status = ImportStatus::Unsupported;
    result.message = "no importer claims this file type";
    return result;  // AC-44: NOTHING was read; `bytes` was never even looked at
}

}  // namespace engine::editor
