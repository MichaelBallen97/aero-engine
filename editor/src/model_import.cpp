// Aero Engine — model_import.cpp: the pure dispatch and URI-policy half of the model importer (task
// 3.2.1). fastgltf-free AT SOURCE: this TU includes only "gltf_import.hpp" (the src-private
// DECLARATION) and calls importGltf(); the glTF backend itself lives entirely in gltf_import.cpp.
// NOTHING HERE LOGS (INV-A3), NOTHING HERE TOUCHES DISK (INV-M3), NOTHING HERE THROWS.
#include <aero/editor/model_import.hpp>

#include "gltf_import.hpp"

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
    constexpr std::array<std::string_view, 2> EXTENSIONS = {".gltf", ".glb"};
    for (const std::string_view ext : EXTENSIONS) {
        if (fileName.size() <= ext.size()) {
            continue;  // the isMetaFileName shape: ".gltf" alone needs something BEFORE the extension
        }
        const std::size_t offset = fileName.size() - ext.size();
        bool matches = true;
        for (std::size_t i = 0; i < ext.size(); ++i) {
            const auto a = static_cast<unsigned char>(fileName[offset + i]);
            const auto b = static_cast<unsigned char>(ext[i]);
            if (foldAscii(a) != foldAscii(b)) {
                matches = false;
                break;
            }
        }
        if (matches) {
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
    if (!isImportableModelName(fileName)) {
        ImportResult result;
        result.status = ImportStatus::Unsupported;
        result.message = "no importer claims this file type";
        return result;  // AC-44: NOTHING was read; `bytes` was never even looked at
    }
    // 3.2.2 (ufbx) adds `if (hasExtension(fileName, ".fbx")) { return importFbx(...); }` HERE, beside
    // this one, with its own TU. Do not turn this into a table until there are three arms.
    return importGltf(assetRelativeDir, bytes, settings, depth, external);
}

}  // namespace engine::editor
