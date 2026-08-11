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
#include <cstddef>
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
    constexpr std::array<std::string_view, 5> EXTENSIONS = {".gltf", ".glb", ".fbx", ".obj", ".mtl"};
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
