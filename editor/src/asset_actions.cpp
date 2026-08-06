// Aero Engine — the orphan-sidecar delete action (task 3.1.3). The FIFTH editor/src TU to include
// <filesystem>, and it holds EXACTLY ONE std::filesystem::remove call (§V6 greps both facts). NEVER
// LOGS (INV-V8/INV-A3's posture extended here): every exit is a RESULT, never a printed line.
#include <aero/editor/asset_actions.hpp>
#include <aero/editor/asset_meta.hpp>
#include <aero/editor/project_files.hpp>
#include <aero/editor/text_file.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace engine::editor {

namespace {

// project_files.cpp:29-33's precedent, copied TU-locally: construct from UTF-8 BYTES so non-ASCII
// names resolve correctly on Windows, where path's native encoding is UTF-16 and the narrow-char
// constructor assumes the active code page (NOT UTF-8).
std::filesystem::path pathFromUtf8(std::string_view utf8) {
    const std::u8string bytes(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(bytes);
}

// code-review finding 5: a pure STRING check, deliberately never std::filesystem::path::is_absolute()
// -- project.hpp:90-93's own A19 rationale, copied here: on Windows is_absolute("/shared") is FALSE
// (no root name), so a POSIX-rooted value would sail through a check meant to require ONE. Non-empty,
// and starts with '/' (POSIX, and the forward-slash form this tree's own roots already use) or a
// two-character ASCII drive-letter prefix ("C:", whatever follows). Deliberately NOT full UNC/registry
// validation -- this is a REFUSAL gate for an obviously-wrong root (an empty string, or a bare relative
// path), not a general path-legality checker; validateOrphanPath above already owns the relative half.
bool looksLikeAnAbsoluteRoot(std::string_view root) noexcept {
    if (root.empty()) {
        return false;
    }
    if (root.front() == '/') {
        return true;
    }
    if (root.size() >= 2) {
        const char first = root[0];
        const bool isAsciiLetter = (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z');
        if (isAsciiLetter && root[1] == ':') {
            return true;
        }
    }
    return false;
}

}  // namespace

OrphanDeleteRefusal validateOrphanPath(std::string_view relativeMetaPath) noexcept {
    const std::string_view leaf = leafOf(relativeMetaPath);
    if (!isMetaFileName(leaf)) {
        return OrphanDeleteRefusal::NotAMetaName;  // covers an empty path too (leafOf("") == "")
    }
    for (const char c : relativeMetaPath) {
        if (c == '\\') {
            return OrphanDeleteRefusal::EscapesRoot;  // a Windows separator must never reach a
                                                      // relative key (2.2.4's paths are '/'-only)
        }
    }
    if (relativeMetaPath.front() == '/') {
        return OrphanDeleteRefusal::EscapesRoot;  // an absolute POSIX path
    }
    if (relativeMetaPath.size() >= 2) {
        const char first = relativeMetaPath[0];
        const bool isAsciiLetter = (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z');
        if (isAsciiLetter && relativeMetaPath[1] == ':') {
            return OrphanDeleteRefusal::EscapesRoot;  // a rooted Windows drive letter ("C:...")
        }
    }
    // ANY ".." SEGMENT, not merely the substring -- "..foo" and "foo.." are legal leaf names.
    // Pointer+length construction throughout (never substr): this function is noexcept, and substr
    // may throw std::out_of_range -- bugprone-exception-escape (asset_view.cpp's rawExtensionOf
    // precedent, task 3.1.3's own earlier fix for the identical trap).
    std::size_t start = 0;
    while (start <= relativeMetaPath.size()) {
        const std::size_t slash = relativeMetaPath.find('/', start);
        const std::size_t end = slash == std::string_view::npos ? relativeMetaPath.size() : slash;
        const std::string_view segment(relativeMetaPath.data() + start, end - start);
        if (segment == "..") {
            return OrphanDeleteRefusal::EscapesRoot;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return OrphanDeleteRefusal::None;
}

OrphanDeleteResult deleteOrphanMeta(std::string_view assetsRootUtf8, std::string_view relativeMetaPath) {
    OrphanDeleteResult result;

    // 0: refuse an empty or non-absolute root EXPLICITLY, before it is ever concatenated into a path
    // (code-review finding 5). An empty root plus "/" plus a relative path resolves to the filesystem
    // ROOT ("" + "/" + "wood.png.meta" -> "/wood.png.meta") -- AssetDatabase::root() is genuinely empty
    // with no project open, and this function has no other guard against that. Reusing EscapesRoot: an
    // unresolvable root is conceptually the same failure as a path that escapes a real one.
    if (!looksLikeAnAbsoluteRoot(assetsRootUtf8)) {
        result.refusal = OrphanDeleteRefusal::EscapesRoot;
        result.message = "the assets root is empty or not absolute";
        return result;
    }

    // 1: refuse before any disk touch (E24).
    const OrphanDeleteRefusal pathRefusal = validateOrphanPath(relativeMetaPath);
    if (pathRefusal != OrphanDeleteRefusal::None) {
        result.refusal = pathRefusal;
        result.message = "not a safe sidecar path";
        return result;
    }

    const std::string absoluteMetaPath = std::string(assetsRootUtf8) + "/" + std::string(relativeMetaPath);

    // 2: Missing is NOT an error condition beyond the log line the caller writes (E21).
    if (!fileExists(absoluteMetaPath)) {
        result.refusal = OrphanDeleteRefusal::Missing;
        result.message = "the sidecar no longer exists";
        return result;
    }

    // 3: the action never deletes on a name alone -- a file literally called "notes.meta" that is
    // not a real sidecar survives (E23).
    const FileReadResult read = readTextFile(absoluteMetaPath);
    if (!read.text.has_value()) {
        result.refusal = OrphanDeleteRefusal::NotAMeta;
        result.message = "could not be read: " + read.error;
        return result;
    }
    const MetaParseResult parsed = parseMeta(*read.text);
    if (!parsed.guid.has_value()) {
        result.refusal = OrphanDeleteRefusal::NotAMeta;
        result.message = "does not parse as a .meta v1 sidecar";
        return result;
    }

    // 4: the check that stops a race from destroying a live identity (E22).
    const std::string_view metaLeaf = leafOf(relativeMetaPath);
    const std::string_view assetLeaf = assetNameForMeta(metaLeaf);
    const std::string assetRelPath = joinRelative(parentOf(relativeMetaPath), assetLeaf);
    const std::string absoluteAssetPath = std::string(assetsRootUtf8) + "/" + assetRelPath;
    if (fileExists(absoluteAssetPath)) {
        result.refusal = OrphanDeleteRefusal::AssetPresent;
        result.message = "the asset it describes exists again";
        return result;
    }

    // 5: the ONE sanctioned std::filesystem::remove in the editor.
    std::error_code ec;
    const bool removed = std::filesystem::remove(pathFromUtf8(absoluteMetaPath), ec);
    if (ec || !removed) {
        result.refusal = OrphanDeleteRefusal::RemoveFailed;
        result.message = ec ? ec.message() : std::string("the OS refused to remove the file");
        return result;
    }

    // 6.
    result.deleted = true;
    return result;
}

}  // namespace engine::editor
