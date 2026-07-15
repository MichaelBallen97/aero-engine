#pragma once
// Aero Engine — the virtual file system (task 0.2.6): the `vfs` half of core's Time & VFS deliverable.
// A VFS resolves engine-virtual paths ("res://textures/wood.png") to bytes, independently of where
// those bytes actually live. In the editor/dev they are loose files in the project directory; in an
// exported game they are slices of a single game.pak archive (task 5.1.1). Game and engine code read
// the SAME virtual paths in both cases — the VFS swaps the backend underneath, so nothing above it
// knows or cares. That transparent swap is the whole reason this abstraction exists (docs/03: the
// runtime's ".pak loading" mounts through the VFS).
//
// Path scheme: "res://" is the read-only game-data root (Godot's convention — self-documenting). A
// future writable "user://" root (save games, config) is reserved by name but NOT implemented here:
// no consumer needs writes within Phase 0-5's core loop, and read-only matches the .pak's nature.
//
// Boundary/hygiene: this public header exposes only engine and standard-library vocabulary types — it
// does NOT include <filesystem>. All std::filesystem / std::fstream usage is confined to vfs.cpp, so
// the subsystems that include this header (assets, script, render, ...) don't drag the filesystem
// library onto their compile line. std::filesystem is stdlib, not third-party, so this is hygiene,
// not the boundary rule — but the same instinct. core depends on NOTHING: the VFS is built on the
// standard library, so it adds no vcpkg port and needs no boundary guard.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine {

// The byte blob a file read yields. std::byte (not char/unsigned char) states "opaque bytes, not
// text" at the type level; readText() is the string-typed convenience on top.
using ByteBuffer = std::vector<std::byte>;

// The backend seam. One VFS mount is one FileSystemBackend; the VFS routes reads to it. Today the
// only implementation is DirectoryBackend (loose files); task 5.1.1 adds a PakBackend (archive
// slices) that plugs in here with no change to callers. The single virtual call per file op is
// nothing beside the disk/archive access it precedes.
//
// relPath is ALWAYS a VFS-normalized, root-relative path: no scheme, no leading '/', no "."/".."
// segments (the VFS guarantees this before calling a backend) — e.g. "textures/wood.png".
class FileSystemBackend {
public:
    FileSystemBackend() = default;
    virtual ~FileSystemBackend() = default;

    // Polymorphic base: non-copyable, non-movable, so a backend is only ever handled through the
    // unique_ptr the VFS owns it by (no slicing).
    FileSystemBackend(const FileSystemBackend&) = delete;
    FileSystemBackend& operator=(const FileSystemBackend&) = delete;
    FileSystemBackend(FileSystemBackend&&) = delete;
    FileSystemBackend& operator=(FileSystemBackend&&) = delete;

    [[nodiscard]] virtual bool exists(std::string_view relPath) const = 0;
    [[nodiscard]] virtual std::optional<std::uint64_t> fileSize(std::string_view relPath) const = 0;
    [[nodiscard]] virtual std::optional<ByteBuffer> readFile(std::string_view relPath) const = 0;
};

// A backend mapping virtual paths onto loose files under one real directory (the dev/editor path).
// Construct it with a real, UTF-8-encoded directory path; it is stored as-is and joined with each
// relPath inside vfs.cpp. A relPath that is absolute or contains a ".." segment is refused (defense
// in depth — the VFS already strips both before a backend sees a path).
class DirectoryBackend final : public FileSystemBackend {
public:
    explicit DirectoryBackend(std::string_view rootDirectory);

    [[nodiscard]] bool exists(std::string_view relPath) const override;
    [[nodiscard]] std::optional<std::uint64_t> fileSize(std::string_view relPath) const override;
    [[nodiscard]] std::optional<ByteBuffer> readFile(std::string_view relPath) const override;

private:
    std::string rootDir;  // real, UTF-8 directory path; converted to std::filesystem::path in vfs.cpp
};

// The front door. Mount one or more backends, then read through virtual paths.
//
// Overlay semantics: mounts are searched most-recently-mounted first, falling through to earlier
// mounts on a miss — so a later mount overrides (shadows) an earlier one for the paths it provides.
// Phase 0 mounts exactly one directory at the res:// root; the ordering is defined now for the .pak
// mount (5.1.1) and future patch/DLC mounts.
//
// Thread-safety: mount()/clear() mutate the mount table and are NOT thread-safe — do them at setup,
// on one thread. The read queries (exists/fileSize/readFile/readText) are const and safe to call
// concurrently from many threads afterward (each backend read is independent, holding no shared
// mutable state) — the contract Phase 3's job-driven async asset loading relies on.
//
// Errors: reads return std::optional; std::nullopt means "absent OR unreadable" — the two are not
// distinguished (no exceptions cross this API, docs/04; std::expected is C++23, past the C++20
// baseline). A caller that must tell them apart checks exists() first.
class VirtualFileSystem {
public:
    // Mount at the res:// root (the common case). A null backend is ignored.
    void mount(std::unique_ptr<FileSystemBackend> backend);
    // Mount at a virtual sub-prefix ("res://dlc"): the backend then provides res://dlc/... A null
    // backend or an unaddressable prefix is ignored.
    void mount(std::string_view virtualPrefix, std::unique_ptr<FileSystemBackend> backend);
    // Unmount everything.
    void clear() noexcept;

    [[nodiscard]] bool exists(std::string_view virtualPath) const;
    [[nodiscard]] std::optional<std::uint64_t> fileSize(std::string_view virtualPath) const;
    [[nodiscard]] std::optional<ByteBuffer> readFile(std::string_view virtualPath) const;
    // readFile + interpret the bytes as a string, with no encoding validation (the raw bytes become
    // the string's contents). For text assets: JS module source (4.1.2), JSON, config.
    [[nodiscard]] std::optional<std::string> readText(std::string_view virtualPath) const;

private:
    struct Mount {
        std::string prefix;  // normalized, root-relative ("" == the res:// root)
        std::unique_ptr<FileSystemBackend> backend;
    };
    std::vector<Mount> mounts;  // searched back-to-front (most recent wins)
};

}  // namespace engine
