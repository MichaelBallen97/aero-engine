// Aero Engine — the only place the editor's text files touch the disk (task 2.5.1 D12/D19/F16/F17;
// promoted to its own header at task 2.6.1, D12). THE ONLY editor TU under this task that includes
// <filesystem>/<fstream>. NEVER THROWS: every call uses the std::error_code overload
// (project_files.cpp's E20 rule). NOTHING HERE LOGS -- status is RETURNED, never printed
// (project_files.hpp:15-16's convention, applied a second time): the caller (scene_session.cpp's
// openSceneFile/saveSceneFile, project_file.cpp's loadProjectFrom/createProject) knows both the path
// and the outcome, this TU only the outcome.
#include <aero/core/content_hash.hpp>
#include <aero/editor/text_file.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace engine::editor {

namespace {

// A third TU-local copy of engine/core/src/vfs.cpp:94-96 / editor/src/project_files.cpp:29-33 (plan
// A5). NOT std::filesystem::u8path: deprecated since C++20 ([depr.fs.path.factory]), and
// clang-tidy's --warnings-as-errors would flag it. Construct from UTF-8 BYTES so non-ASCII names
// resolve correctly on Windows, where path's native encoding is UTF-16 and the narrow-char
// constructor assumes the active code page (NOT UTF-8). Carries no lint suppression:
// project_files.cpp's identical casts carry none either, and cppcoreguidelines-* is not enabled
// (.clang-tidy).
std::filesystem::path pathFromUtf8(std::string_view utf8) {
    const std::u8string bytes(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(bytes);
}

// SCREAMING_SNAKE at file scope: ConstexprVariableCase is UPPER_CASE and applies to file-scope
// constants too. Kept in THIS TU (which includes no SDL) rather than file_dialog.cpp, which can pull
// Windows headers in through SDL -- <windows.h> does not define TEMP_SUFFIX, but there is no reason to
// take the risk (plan R13).
constexpr std::string_view TEMP_SUFFIX = ".aero-tmp";

}  // namespace

FileReadResult readTextFile(std::string_view absolutePathUtf8) {
    std::error_code ec;
    const std::filesystem::path fsPath = pathFromUtf8(absolutePathUtf8);
    if (std::filesystem::is_directory(fsPath, ec)) {
        return {std::nullopt, "path is a directory"};
    }
    // BINARY on BOTH sides (also in writeTextFileAtomic below): text mode on Windows turns every '\n'
    // into "\r\n" on write and back on '\n' on read, which would make a scene saved on Windows differ
    // byte-for-byte from the same scene saved on macOS/Linux -- and 2.5.2 is a BYTE comparison
    // (F19/H1/INV-8). This is the single most load-bearing line in this file.
    std::ifstream in(fsPath, std::ios::binary);
    if (!in) {
        return {std::nullopt, std::strerror(errno)};
    }
    std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    // .bad(), NOT .fail(): fail() is set by the EOF that ends the istreambuf_iterator loop above, which
    // is the ordinary, successful termination -- not a read error.
    if (in.bad()) {
        return {std::nullopt, "read failed"};
    }
    return {std::move(text), {}};
}

std::string writeTextFileAtomic(std::string_view absolutePathUtf8, std::string_view text) {
    const std::filesystem::path target = pathFromUtf8(absolutePathUtf8);
    std::filesystem::path temp = target;
    temp += TEMP_SUFFIX;
    {
        // SCOPED: the stream must be CLOSED before the rename below -- Windows refuses to replace an
        // OPEN file ([[windows-cannot-remove-an-open-file]] applies to rename() for the same reason).
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return std::strerror(errno);
        }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) {
            std::error_code drop;
            std::filesystem::remove(temp, drop);
            return "write failed";
        }
    }
    std::error_code ec;
    std::filesystem::rename(temp, target, ec);  // replaces an existing target on all three OSes (A27)
    if (ec) {
        std::error_code drop;
        std::filesystem::remove(temp, drop);
        return ec.message();
    }
    return {};  // "" == success
}

bool fileExists(std::string_view absolutePathUtf8) {
    std::error_code ec;
    return std::filesystem::exists(pathFromUtf8(absolutePathUtf8), ec);
}

FileHashResult hashFileContents(std::string_view absolutePathUtf8) {
    std::error_code ec;
    const std::filesystem::path fsPath = pathFromUtf8(absolutePathUtf8);
    if (std::filesystem::is_directory(fsPath, ec)) {
        return {std::nullopt, "path is a directory", 0};
    }
    // BINARY, exactly like readTextFile above: text mode on Windows would rewrite "\r\n", making the
    // same file hash differently there (TF17).
    std::ifstream in(fsPath, std::ios::binary);
    if (!in) {
        return {std::nullopt, std::strerror(errno), 0};
    }

    // One heap allocation per call, sized at the CHUNK, not the file: a 2 GB source never becomes a
    // 2 GB buffer. Deliberately NOT a `static thread_local` -- hidden state in a leaf primitive.
    std::vector<char> buffer(HASH_CHUNK_BYTES);
    ContentHasher hasher;
    std::uint64_t bytesRead = 0;
    while (true) {
        in.read(buffer.data(), static_cast<std::streamsize>(HASH_CHUNK_BYTES));
        const std::streamsize got = in.gcount();
        if (got <= 0) {
            break;
        }
        hasher.update(std::as_bytes(std::span<const char>(buffer.data(), static_cast<std::size_t>(got))));
        bytesRead += static_cast<std::uint64_t>(got);
    }
    // .bad(), NOT .fail(): fail() is set by the EOF that ends the loop above, which is the ordinary,
    // successful termination -- not a read error (readTextFile's rule above, applied here too).
    if (in.bad()) {
        return {std::nullopt, "read failed", bytesRead};
    }
    return {hasher.finish(), {}, bytesRead};
}

FileBytesResult readFileBytes(std::string_view absolutePathUtf8, std::uint64_t maxBytes) {
    FileBytesResult result;
    std::error_code ec;
    const std::filesystem::path fsPath = pathFromUtf8(absolutePathUtf8);
    // file_size(directory) is unspecified by the standard and does NOT reliably set `ec` on every
    // platform (verified: it happily returns a directory's own on-disk size on some POSIX libcs) --
    // the readTextFile precedent's explicit is_directory guard, applied here first.
    if (std::filesystem::is_directory(fsPath, ec)) {
        result.error = "path is a directory";
        return result;
    }
    const std::uintmax_t rawSize = std::filesystem::file_size(fsPath, ec);
    if (ec) {
        result.error = ec.message();
        return result;  // missing, or unreadable
    }
    result.size = static_cast<std::uint64_t>(rawSize);
    if (result.size > maxBytes) {
        result.error = "file is too large";
        result.refusedByCap = true;  // code-review finding 6: the DISCRIMINATED signal, not the string
        return result;               // `size` stays filled (seed S33) -- the caller can report what tripped the cap
    }

    // BINARY, exactly like readTextFile/hashFileContents above.
    std::ifstream in(fsPath, std::ios::binary);
    if (!in) {
        result.error = std::strerror(errno);
        return result;
    }
    // resize + read, NOT istreambuf_iterator (§D-4): the allocation is EXACT, and a truncated read is
    // detectable via in.gcount() rather than silently accepting a short buffer.
    std::string data;
    data.resize(static_cast<std::size_t>(result.size));
    if (result.size > 0) {
        in.read(data.data(), static_cast<std::streamsize>(result.size));
        if (static_cast<std::uint64_t>(in.gcount()) != result.size) {
            result.error = "read failed";
            return result;
        }
    }
    if (in.bad()) {
        result.error = "read failed";
        return result;
    }
    result.bytes = std::move(data);
    return result;
}

std::string ensureDirectory(std::string_view absolutePathUtf8) {
    std::error_code ec;
    const std::filesystem::path p = pathFromUtf8(absolutePathUtf8);
    std::filesystem::create_directories(p, ec);  // the BOOL RETURN IS DELIBERATELY IGNORED (2.6.1 S22)
    if (!ec) {
        return {};
    }
    std::error_code dirEc;  // a race: another process created it between the two calls above
    if (std::filesystem::is_directory(p, dirEc) && !dirEc) {
        return {};
    }
    return ec.message();
}

}  // namespace engine::editor
