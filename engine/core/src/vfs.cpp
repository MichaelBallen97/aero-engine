#include <aero/core/vfs.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace engine {
namespace {

constexpr std::string_view SCHEME = "res://";

// Turn "res://a/./b/../c" into "a/c". Returns nullopt when the path is not addressable:
//   * it does not start with "res://"
//   * it contains a backslash or a NUL (a virtual path is always '/'-separated and NUL-free)
//   * a ".." segment would escape above the root
// The result has no scheme, no leading/trailing '/', and no "."/".." segments; "" is the root
// itself. Normalizing HERE — before any backend sees the path — is what makes a "../" escape and a
// stray separator structurally impossible to turn into a disk access outside a mount root (D13).
std::optional<std::string> normalizeVirtualPath(std::string_view path) {
    if (path.substr(0, SCHEME.size()) != SCHEME) {
        return std::nullopt;
    }
    const std::string_view rel = path.substr(SCHEME.size());
    if (rel.find('\\') != std::string_view::npos || rel.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }

    std::vector<std::string_view> segments;
    std::size_t cursor = 0;
    while (cursor <= rel.size()) {
        const std::size_t slash = rel.find('/', cursor);
        const std::size_t end = (slash == std::string_view::npos) ? rel.size() : slash;
        const std::string_view segment = rel.substr(cursor, end - cursor);

        if (segment == "..") {
            if (segments.empty()) {
                return std::nullopt;  // escapes above the root
            }
            segments.pop_back();
        } else if (!segment.empty() && segment != ".") {
            segments.push_back(segment);
        }
        // an empty segment ("//") and "." fall through as no-ops

        if (slash == std::string_view::npos) {
            break;
        }
        cursor = end + 1;
    }

    std::string out;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (i != 0) {
            out.push_back('/');
        }
        out.append(segments[i]);
    }
    return out;
}

// "Is `path` at or below `prefix`?" — and if so, the remainder relative to it. prefix "" (the root)
// matches everything. Routes a normalized virtual path to the mount that provides it.
std::optional<std::string> subpathUnder(const std::string& prefix, const std::string& path) {
    if (prefix.empty()) {
        return path;
    }
    if (path == prefix) {
        return std::string{};
    }
    if (path.size() > prefix.size() && path.compare(0, prefix.size(), prefix) == 0 && path[prefix.size()] == '/') {
        return path.substr(prefix.size() + 1);
    }
    return std::nullopt;
}

// Construct a std::filesystem::path from UTF-8 bytes so non-ASCII names resolve correctly on Windows,
// where path's native encoding is UTF-16 and the narrow-char constructor assumes the active code page
// (NOT UTF-8). Going through char8_t/u8string selects the UTF-8-aware conversion on every OS (D12).
std::filesystem::path pathFromUtf8(std::string_view utf8) {
    const std::u8string bytes(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(bytes);
}

// True if any '/'-separated segment of relPath is exactly "..".
bool hasDotDotSegment(std::string_view relPath) {
    std::size_t cursor = 0;
    while (cursor <= relPath.size()) {
        const std::size_t slash = relPath.find('/', cursor);
        const std::size_t end = (slash == std::string_view::npos) ? relPath.size() : slash;
        if (relPath.substr(cursor, end - cursor) == "..") {
            return true;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        cursor = end + 1;
    }
    return false;
}

// Resolve a root-relative path to a real path under `root`, or nullopt if it would escape. relPath
// arrives clean from the VFS; the is_absolute()/".." guards defend DIRECT DirectoryBackend use (an
// absolute path would replace the root via operator/=, and a "../" would climb out). The guard is
// lexical: it does not resolve symlinks, so a symlink placed inside a mounted directory can still
// point outside — acceptable here (the editor mounts the user's own project; the exported .pak has
// no symlinks).
std::optional<std::filesystem::path> resolveWithin(const std::filesystem::path& root, std::string_view relPath) {
    const std::filesystem::path relative = pathFromUtf8(relPath);
    if (relative.is_absolute() || hasDotDotSegment(relPath)) {
        return std::nullopt;
    }
    return root / relative;
}

}  // namespace

// ---- DirectoryBackend --------------------------------------------------------------------------

DirectoryBackend::DirectoryBackend(std::string_view rootDirectory) : rootDir(rootDirectory) {}

bool DirectoryBackend::exists(std::string_view relPath) const {
    const std::optional<std::filesystem::path> full = resolveWithin(pathFromUtf8(rootDir), relPath);
    if (!full) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::is_regular_file(*full, ec) && !ec;
}

std::optional<std::uint64_t> DirectoryBackend::fileSize(std::string_view relPath) const {
    const std::optional<std::filesystem::path> full = resolveWithin(pathFromUtf8(rootDir), relPath);
    if (!full) {
        return std::nullopt;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(*full, ec) || ec) {
        return std::nullopt;
    }
    const std::uintmax_t size = std::filesystem::file_size(*full, ec);
    if (ec) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(size);
}

std::optional<ByteBuffer> DirectoryBackend::readFile(std::string_view relPath) const {
    const std::optional<std::filesystem::path> full = resolveWithin(pathFromUtf8(rootDir), relPath);
    if (!full) {
        return std::nullopt;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(*full, ec) || ec) {
        return std::nullopt;  // missing, a directory, or unstattable
    }
    const std::uintmax_t size = std::filesystem::file_size(*full, ec);
    if (ec) {
        return std::nullopt;
    }

    std::ifstream stream(*full, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }

    ByteBuffer buffer(static_cast<std::size_t>(size));
    if (size > 0) {
        // std::byte* -> char* is the unavoidable cast for stream I/O over raw bytes.
        stream.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(size));
        if (stream.gcount() != static_cast<std::streamsize>(size)) {
            return std::nullopt;  // short read — the file changed under us, or an I/O error
        }
    }
    return buffer;
}

// ---- VirtualFileSystem -------------------------------------------------------------------------

void VirtualFileSystem::mount(std::unique_ptr<FileSystemBackend> backend) {
    if (backend) {
        mounts.push_back(Mount{std::string{}, std::move(backend)});
    }
}

void VirtualFileSystem::mount(std::string_view virtualPrefix, std::unique_ptr<FileSystemBackend> backend) {
    if (!backend) {
        return;
    }
    std::string prefix;
    if (!virtualPrefix.empty()) {
        const std::optional<std::string> normalized = normalizeVirtualPath(virtualPrefix);
        if (!normalized) {
            return;  // an unaddressable prefix mounts nothing
        }
        prefix = *normalized;
    }
    mounts.push_back(Mount{std::move(prefix), std::move(backend)});
}

void VirtualFileSystem::clear() noexcept { mounts.clear(); }

bool VirtualFileSystem::exists(std::string_view virtualPath) const {
    const std::optional<std::string> normalized = normalizeVirtualPath(virtualPath);
    if (!normalized) {
        return false;
    }
    for (const Mount& mount : std::ranges::reverse_view(mounts)) {
        const std::optional<std::string> sub = subpathUnder(mount.prefix, *normalized);
        if (sub && mount.backend->exists(*sub)) {
            return true;
        }
    }
    return false;
}

std::optional<std::uint64_t> VirtualFileSystem::fileSize(std::string_view virtualPath) const {
    const std::optional<std::string> normalized = normalizeVirtualPath(virtualPath);
    if (!normalized) {
        return std::nullopt;
    }
    for (const Mount& mount : std::ranges::reverse_view(mounts)) {
        const std::optional<std::string> sub = subpathUnder(mount.prefix, *normalized);
        if (!sub) {
            continue;
        }
        if (std::optional<std::uint64_t> size = mount.backend->fileSize(*sub)) {
            return size;
        }
    }
    return std::nullopt;
}

std::optional<ByteBuffer> VirtualFileSystem::readFile(std::string_view virtualPath) const {
    const std::optional<std::string> normalized = normalizeVirtualPath(virtualPath);
    if (!normalized) {
        return std::nullopt;
    }
    for (const Mount& mount : std::ranges::reverse_view(mounts)) {
        const std::optional<std::string> sub = subpathUnder(mount.prefix, *normalized);
        if (!sub) {
            continue;
        }
        if (std::optional<ByteBuffer> data = mount.backend->readFile(*sub)) {
            return data;
        }
    }
    return std::nullopt;
}

std::optional<std::string> VirtualFileSystem::readText(std::string_view virtualPath) const {
    const std::optional<ByteBuffer> bytes = readFile(virtualPath);
    if (!bytes) {
        return std::nullopt;
    }
    if (bytes->empty()) {
        return std::string{};
    }
    // byte* -> const char* to reinterpret the blob as text; std::string is byte-based (no encoding
    // validation — D16).
    return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}

}  // namespace engine
