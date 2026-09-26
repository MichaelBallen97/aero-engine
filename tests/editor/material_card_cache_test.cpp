// tests/editor/material_card_cache_test.cpp -- task E.4.5: the material CARD cache over a REAL scanned project
// (CC1-CC10). A TU of aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// TIER 0 AND UNGATED: no GPU, no window, no ImGui. It reads real files, because AssetDatabase has no setter --
// a record with a real GUID and a real content hash only comes from a real scan (inspector_test.cpp's
// ScannedAssets precedent). Every fixture lives under the OS temp directory, remove_all'd FIRST, so a second
// run cannot inherit the first's tree; run it with a private TMPDIR, because two concurrent runs of this
// binary build the same paths. NO PREPROCESSOR CONDITIONAL OF ANY KIND, NO ENTROPY SOURCE.
//
// It includes two SRC-PRIVATE headers, both ImGui-free (blender_service_test.cpp's precedent): the cache
// itself, and ThumbnailService for CC9 -- the ONE place in the tree that can prove the card pass sits ABOVE the
// device gate, because it can construct the service with NO device at all.
#include "../../editor/src/material_card_cache.hpp"

#include <aero/core/content_hash.hpp>
#include <aero/core/guid.hpp>
#include <aero/editor/asset_database.hpp>
#include <aero/editor/asset_meta.hpp>
#include <aero/editor/material_card.hpp>
#include <aero/editor/project_files.hpp>
#include <aero/editor/text_file.hpp>
#include <aero/editor/thumbnail_cache.hpp>
#include <aero/reflect/material_format.hpp>

#include "../../editor/src/thumbnail_service.hpp"

#include <doctest/doctest.h>

#include <cstddef>  // task E.4.5 review -- std::byte, for CC14
#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>  // MSVC alone needs the complete type to stringify a string_view inside a CHECK
#include <span>     // task E.4.5 review -- the one-shot hash over the read bytes (CC14)
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using engine::MaterialDocument;
using engine::editor::AssetRecord;
using engine::editor::MaterialCard;
using engine::editor::MaterialCardCache;
using engine::editor::ThumbnailKey;

namespace {

[[nodiscard]] std::string utf8Of(const std::filesystem::path& path) {
    const std::u8string bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// A canonical dielectric with a name -- writeMaterialText's own output, so no unknown key and no WARN.
[[nodiscard]] std::string materialText(std::string_view name, float grey) {
    MaterialDocument document;
    document.name = std::string(name);
    document.baseColorFactor = engine::Vec4{grey, grey, grey, 1.0F};
    document.metallicFactor = 0.0F;
    return engine::writeMaterialText(document);
}

struct CardProject {
    std::filesystem::path dir;
    std::string projectRoot;
    std::string assetsRoot;
    engine::GuidGenerator generator{0xCC45E45ULL};
    engine::editor::AssetDatabase database;
};

void rescan(CardProject& project) {
    const engine::editor::AssetScanReport report =
        project.database.rescan(project.projectRoot, project.assetsRoot, project.generator);
    REQUIRE(report.status == engine::editor::ScanStatus::Ok);
}

void writeFile(const CardProject& project, const std::string& relativePath, std::string_view bytes) {
    REQUIRE(engine::editor::writeTextFileAtomic(project.assetsRoot + "/" + relativePath, bytes).empty());
}

// {relative path, bytes} pairs. A named alias so no signature or fixture below needs a forced line break.
using FileList = std::vector<std::pair<std::string, std::string>>;

[[nodiscard]] std::unique_ptr<CardProject> makeCardProject(const FileList& files) {
    static int counter = 0;
    auto project = std::make_unique<CardProject>();
    project->dir = std::filesystem::temp_directory_path() / ("aero_card_cache_" + std::to_string(++counter));
    std::error_code ec;
    std::filesystem::remove_all(project->dir, ec);
    std::filesystem::create_directories(project->dir / "assets", ec);
    REQUIRE_FALSE(ec);
    project->projectRoot = utf8Of(project->dir);
    project->assetsRoot = utf8Of(project->dir / "assets");
    for (const auto& [relativePath, bytes] : files) {
        writeFile(*project, relativePath, bytes);
    }
    rescan(*project);
    return project;
}

[[nodiscard]] ThumbnailKey keyOf(const CardProject& project, std::string_view relativePath) {
    const AssetRecord* const record = project.database.findByPath(relativePath);
    REQUIRE(record != nullptr);
    const std::optional<ThumbnailKey> key = engine::editor::thumbnailKeyForRecord(*record);
    REQUIRE(key.has_value());
    return *key;
}

// a.aeromat, b.aeromat and c.aeromat -- CC7's and CC8's fixture.
[[nodiscard]] FileList threeMaterialFiles() {
    FileList files;
    files.emplace_back("a.aeromat", materialText("A", 0.5F));
    files.emplace_back("b.aeromat", materialText("B", 0.5F));
    files.emplace_back("c.aeromat", materialText("C", 0.5F));
    return files;
}

}  // namespace

TEST_CASE("material card cache: a wanted material is read once into a sanitised card (CC1)") {
    const std::string text = materialText("  Studio\tBrass ", 0.5F);  // a tab and a trailing space: rules 1-2
    const std::unique_ptr<CardProject> project = makeCardProject({{"brass.aeromat", text}});
    const ThumbnailKey key = keyOf(*project, "brass.aeromat");
    MaterialCardCache cache;
    CHECK(cache.cardFor(key) == nullptr);
    cache.noteCardWanted(key);
    CHECK(cache.cardFor(key) == nullptr);  // NOTING READS NOTHING: a draw walk never performs I/O
    CHECK(cache.readCount() == 0U);
    cache.service(project->database, 1U);
    const MaterialCard* const card = cache.cardFor(key);
    REQUIRE(card != nullptr);
    CHECK(card->displayName == "Studio Brass");  // rules 1-2, applied by the model the cache calls
    CHECK_FALSE(card->displayNameTruncated);
    REQUIRE(card->hasSwatch);
    CHECK(static_cast<int>(card->swatch.r) == 206);  // MB11's oracle for 0.5, through the real read
    CHECK(cache.readCount() == 1U);
    CHECK(cache.cardCount() == 1U);
}

TEST_CASE("material card cache: one read per key, and a new content hash is a new card (CC2)") {
    const std::unique_ptr<CardProject> project = makeCardProject({{"brass.aeromat", materialText("Brass", 0.5F)}});
    const ThumbnailKey first = keyOf(*project, "brass.aeromat");
    MaterialCardCache cache;
    for (std::uint64_t frame = 1; frame <= 10; ++frame) {
        cache.noteCardWanted(first);
        cache.service(project->database, frame);
    }
    CHECK(cache.readCount() == 1U);  // touched nine more times, read once
    // An EDIT is a new content hash, hence a new key, hence a fresh read -- the ledger's invalidation rule,
    // reused. A cache keyed on the GUID alone would keep serving "Brass" (seed S26).
    writeFile(*project, "brass.aeromat", materialText("Polished Gold", 0.5F));
    rescan(*project);
    const ThumbnailKey second = keyOf(*project, "brass.aeromat");
    REQUIRE((second.guid == first.guid));
    REQUIRE_FALSE((second == first));
    cache.noteCardWanted(second);
    cache.service(project->database, 11U);
    CHECK(cache.readCount() == 2U);
    const MaterialCard* const card = cache.cardFor(second);
    REQUIRE(card != nullptr);
    CHECK(card->displayName == "Polished Gold");
}

TEST_CASE("material card cache: an IMAGE's key is never read as a material (CC3)") {
    FileList files;
    files.emplace_back("a.png", "these bytes are never decoded here");
    files.emplace_back("m.aeromat", materialText("M", 0.5F));
    const std::unique_ptr<CardProject> project = makeCardProject(files);
    const ThumbnailKey image = keyOf(*project, "a.png");  // every decodable image HAS a key -- that is the risk
    const ThumbnailKey material = keyOf(*project, "m.aeromat");
    MaterialCardCache cache;
    for (std::uint64_t frame = 1; frame <= 10; ++frame) {
        cache.noteCardWanted(image);
        cache.noteCardWanted(material);
        cache.service(project->database, frame);
    }
    CHECK(cache.cardFor(image) == nullptr);
    CHECK(cache.readCount() == 1U);  // the material's read ONLY -- the image was refused by the gate (seed S11)
    CHECK(cache.cardCount() == 1U);  // ANTI-VACUITY: the same passes DID read a material
}

TEST_CASE("material card cache: at most four reads per tick, and the rest arrive on the next ones (CC4)") {
    FileList files;
    for (int i = 0; i < 6; ++i) {
        files.emplace_back("m" + std::to_string(i) + ".aeromat", materialText("M" + std::to_string(i), 0.5F));
    }
    const std::unique_ptr<CardProject> project = makeCardProject(files);
    std::vector<ThumbnailKey> keys;
    keys.reserve(6U);  // performance-inefficient-vector-operation, an error under CI's clang-tidy
    for (int i = 0; i < 6; ++i) {
        keys.push_back(keyOf(*project, "m" + std::to_string(i) + ".aeromat"));
    }
    MaterialCardCache cache;
    const auto wantAll = [&](std::uint64_t frame) {
        for (const ThumbnailKey& key : keys) {
            cache.noteCardWanted(key);
        }
        cache.service(project->database, frame);
    };
    wantAll(1U);
    // A LITERAL 4 as well as the constant. The constant alone agrees with any retune up to this fixture's
    // six files -- a budget raised to 5 kept this case green -- so the claim in the case's name is pinned
    // by the literal, and a deliberate retune edits this line too (I233's rule for the render budget).
    CHECK(cache.readCount() == 4U);
    CHECK(cache.readCount() == engine::editor::MAX_MATERIAL_CARD_READS_PER_TICK);
    wantAll(2U);
    CHECK(cache.readCount() == 6U);
    wantAll(3U);
    CHECK(cache.readCount() == 6U);
    CHECK(cache.cardCount() == 6U);
}

TEST_CASE("material card cache: a malformed .aeromat is read ONCE and yields no card, for ever (CC5)") {
    const std::unique_ptr<CardProject> project = makeCardProject({{"bad.aeromat", "{ this is not a material"}});
    const ThumbnailKey key = keyOf(*project, "bad.aeromat");
    MaterialCardCache cache;
    for (std::uint64_t frame = 1; frame <= 10; ++frame) {
        cache.noteCardWanted(key);
        cache.service(project->database, frame);
        CHECK(cache.cardFor(key) == nullptr);
    }
    CHECK(cache.readCount() == 1U);  // STICKY per content hash -- never re-read (seed S13)
    CHECK(cache.cardCount() == 0U);
}

TEST_CASE("material card cache: a stale or vanished key reads nothing (CC6)") {
    const std::unique_ptr<CardProject> project = makeCardProject({{"m.aeromat", materialText("M", 0.5F)}});
    const ThumbnailKey live = keyOf(*project, "m.aeromat");
    const ThumbnailKey stale{.guid = live.guid, .hash = engine::ContentHash{.hi = ~live.hash.hi, .lo = live.hash.lo}};
    const ThumbnailKey vanished{.guid = engine::Guid{0xFEEDFACECAFEBEEFULL, 0x0123456789ABCDEFULL}, .hash = live.hash};
    REQUIRE(project->database.findByGuid(vanished.guid) == nullptr);
    MaterialCardCache cache;
    cache.noteCardWanted(stale);
    cache.noteCardWanted(vanished);
    cache.service(project->database, 1U);
    CHECK(cache.readCount() == 0U);  // the bytes on disk are not the bytes either key names
    cache.noteCardWanted(live);
    cache.service(project->database, 2U);
    CHECK(cache.readCount() == 1U);  // ANTI-VACUITY: the live key IS read
}

TEST_CASE("material card cache: the least recently wanted card is evicted past capacity (CC7)") {
    const std::unique_ptr<CardProject> project = makeCardProject(threeMaterialFiles());
    const ThumbnailKey a = keyOf(*project, "a.aeromat");
    const ThumbnailKey b = keyOf(*project, "b.aeromat");
    const ThumbnailKey c = keyOf(*project, "c.aeromat");
    MaterialCardCache cache(2U);
    cache.noteCardWanted(a);
    cache.service(project->database, 1U);
    cache.noteCardWanted(b);
    cache.service(project->database, 2U);
    cache.noteCardWanted(c);
    cache.service(project->database, 3U);
    CHECK(cache.cardCount() == 2U);
    CHECK(cache.cardFor(a) == nullptr);  // wanted longest ago
    CHECK(cache.cardFor(b) != nullptr);
    CHECK(cache.cardFor(c) != nullptr);
    // Eviction really freed it: wanting it again is a fresh READ, not a hit.
    cache.noteCardWanted(a);
    cache.service(project->database, 4U);
    CHECK(cache.readCount() == 4U);
    CHECK(cache.cardFor(a) != nullptr);
    CHECK(cache.cardFor(b) == nullptr);  // now B is the least recently wanted
}

TEST_CASE("material card cache: a card wanted THIS frame is never evicted (CC8)") {
    const std::unique_ptr<CardProject> project = makeCardProject(threeMaterialFiles());
    const ThumbnailKey a = keyOf(*project, "a.aeromat");
    const ThumbnailKey b = keyOf(*project, "b.aeromat");
    const ThumbnailKey c = keyOf(*project, "c.aeromat");
    MaterialCardCache cache(2U);
    cache.noteCardWanted(a);
    cache.noteCardWanted(b);
    cache.noteCardWanted(c);
    cache.service(project->database, 1U);
    CHECK(cache.cardCount() == 3U);  // over capacity for ONE frame, because all three are on screen
    cache.noteCardWanted(a);
    cache.service(project->database, 2U);
    CHECK(cache.cardCount() == 2U);  // ...and back to it on the next, keeping the one still wanted
    CHECK(cache.cardFor(a) != nullptr);
}

TEST_CASE("material card cache: names arrive with NO device at all -- the pass sits above the gate (CC9)") {
    const std::string text = materialText("Studio Brass", 0.5F);
    const std::unique_ptr<CardProject> project = makeCardProject({{"brass.aeromat", text}});
    const ThumbnailKey key = keyOf(*project, "brass.aeromat");
    engine::editor::ThumbnailService service(nullptr);  // no GPU device: every GPU arm is off
    REQUIRE_FALSE(service.available());
    REQUIRE_FALSE(service.materialThumbnailsAvailable());
    service.noteCardWanted(key);
    service.noteVisible(key);  // and the ledger's own queue, which the device gate swallows
    service.service(project->database);
    const MaterialCard* const card = service.cardFor(key);
    REQUIRE(card != nullptr);  // THE CLAIM: below the gate this is null for ever (seed S12)
    CHECK(card->displayName == "Studio Brass");
    CHECK(service.materialCardReadCount() == 1U);
    CHECK(service.readyCount() == 0U);  // ANTI-VACUITY: the ledger really was gated off
    CHECK(service.materialRenderAttempts() == 0U);
}

TEST_CASE("material card cache: clear() drops every card, and the read count never goes back (CC10)") {
    const std::unique_ptr<CardProject> project = makeCardProject({{"m.aeromat", materialText("M", 0.5F)}});
    const ThumbnailKey key = keyOf(*project, "m.aeromat");
    MaterialCardCache cache;
    cache.noteCardWanted(key);
    cache.service(project->database, 1U);
    REQUIRE(cache.cardFor(key) != nullptr);
    cache.clear();
    CHECK(cache.cardFor(key) == nullptr);
    CHECK(cache.cardCount() == 0U);
    CHECK(cache.readCount() == 1U);  // MONOTONIC -- a count that reset would hide a re-read
    cache.noteCardWanted(key);
    cache.service(project->database, 2U);
    CHECK(cache.readCount() == 2U);
    CHECK(cache.cardFor(key) != nullptr);
}

// ---- task E.4.5's code-review round (CC11-CC14): a card is stored ONLY for the bytes its key names ----------
//
// The gate trusts the DATABASE's key, and the database is stale from an external edit until the watcher's
// rescan. So the cache hashes what it actually read and stores a card only when that hash IS the key's; a
// mismatch, or an OS read failure, is answered by the NEXT RESCAN -- never cached, never retried before one.

namespace {

// Moves a file and its sidecar together, the way an external file manager moving an asset with its .meta
// would. OUTSIDE the editor, so nothing rescans.
void moveWithSidecar(const CardProject& project, const std::string& from, const std::string& to) {
    // FROM THE PATH, never from project.assetsRoot: that is UTF-8, and std::filesystem::path's narrow constructor
    // reads it in the ANSI code page on Windows, so a non-ASCII temp directory would name a different folder
    // (the second code-review round). `from` and `to` are ASCII literals, which every code page reads alike.
    const std::filesystem::path assets = project.dir / "assets";
    std::error_code ec;
    std::filesystem::create_directories(assets / to, ec);
    REQUIRE_FALSE(ec);
    std::filesystem::path target = assets / to / std::filesystem::path(from).filename();
    std::filesystem::rename(assets / from, target, ec);
    REQUIRE_FALSE(ec);
    target += ".meta";
    std::filesystem::rename(assets / (from + ".meta"), target, ec);
    REQUIRE_FALSE(ec);
}

}  // namespace

TEST_CASE("material card cache: bytes the key does not name are never cached, nor re-read before a rescan (CC11)") {
    const std::unique_ptr<CardProject> project = makeCardProject({{"brass.aeromat", materialText("Brass", 0.5F)}});
    const ThumbnailKey scanned = keyOf(*project, "brass.aeromat");
    // An external edit the watcher has not reported yet: the database still names the OLD bytes.
    writeFile(*project, "brass.aeromat", materialText("Polished Gold", 0.5F));
    MaterialCardCache cache;
    for (std::uint64_t frame = 1; frame <= 5; ++frame) {
        cache.noteCardWanted(scanned);
        cache.service(project->database, frame);
    }
    CHECK(cache.cardFor(scanned) == nullptr);  // the bytes on disk are NOT the bytes this key names
    CHECK(cache.cardCount() == 0U);
    CHECK(cache.readCount() == 1U);  // read ONCE, then held off: nothing a later service can learn without a rescan
    // The rescan names the new bytes under a NEW key; the stale key is not read at all any more (the gate).
    rescan(*project);
    const ThumbnailKey current = keyOf(*project, "brass.aeromat");
    REQUIRE_FALSE((current == scanned));
    cache.noteCardWanted(scanned);
    cache.service(project->database, 6U);
    CHECK(cache.readCount() == 1U);
    cache.noteCardWanted(current);
    cache.service(project->database, 7U);
    CHECK(cache.readCount() == 2U);
    const MaterialCard* const card = cache.cardFor(current);
    REQUIRE(card != nullptr);
    CHECK(card->displayName == "Polished Gold");
}

TEST_CASE("material card cache: content that returns to its old bytes gets ITS OWN card back (CC12)") {
    // The code-review round's exact scenario: B1 is scanned as {G,H1}; B2 is written and read under {G,H1}
    // before any rescan; a rescan; B1 is written back and rescanned. {G,H1} must name B1 -- a cache that had
    // stored B2's card under {G,H1} would show the wrong name and swatch until the project is reopened.
    const std::unique_ptr<CardProject> project = makeCardProject({{"brass.aeromat", materialText("Brass", 0.5F)}});
    const ThumbnailKey first = keyOf(*project, "brass.aeromat");
    writeFile(*project, "brass.aeromat", materialText("Polished Gold", 0.8F));
    MaterialCardCache cache;
    cache.noteCardWanted(first);
    cache.service(project->database, 1U);
    CHECK(cache.cardFor(first) == nullptr);
    CHECK(cache.readCount() == 1U);
    rescan(*project);
    REQUIRE_FALSE((keyOf(*project, "brass.aeromat") == first));
    writeFile(*project, "brass.aeromat", materialText("Brass", 0.5F));
    rescan(*project);
    REQUIRE((keyOf(*project, "brass.aeromat") == first));  // the same content is the same key again
    cache.noteCardWanted(first);
    cache.service(project->database, 2U);
    CHECK(cache.readCount() == 2U);  // a rescan happened since the refused attempt, so the key is read again
    const MaterialCard* const card = cache.cardFor(first);
    REQUIRE(card != nullptr);
    CHECK(card->displayName == "Brass");             // B1's name -- never B2's
    CHECK(static_cast<int>(card->swatch.r) == 206);  // and B1's swatch (MB11's oracle for 0.5), never B2's
}

TEST_CASE("material card cache: a file missing at its recorded path is retried after the rescan (CC13)") {
    // Moved externally, sidecar with it, before its first read and before any rescan: the read fails on the OS
    // side. That is not a fact about the key's content, so it is never sticky -- the rescan that finds the file
    // at its new path (same GUID, same bytes, so the SAME key) lets the next want read it.
    const std::unique_ptr<CardProject> project =
        makeCardProject({{"brass.aeromat", materialText("Studio Brass", 0.5F)}});
    const ThumbnailKey key = keyOf(*project, "brass.aeromat");
    moveWithSidecar(*project, "brass.aeromat", "mats");
    MaterialCardCache cache;
    for (std::uint64_t frame = 1; frame <= 3; ++frame) {
        cache.noteCardWanted(key);
        cache.service(project->database, frame);
    }
    CHECK(cache.cardFor(key) == nullptr);
    CHECK(cache.readCount() == 1U);  // one attempt, then held off until a rescan
    rescan(*project);
    REQUIRE((keyOf(*project, "mats/brass.aeromat") == key));
    cache.noteCardWanted(key);
    cache.service(project->database, 4U);
    CHECK(cache.readCount() == 2U);
    const MaterialCard* const card = cache.cardFor(key);
    REQUIRE(card != nullptr);
    CHECK(card->displayName == "Studio Brass");
}

TEST_CASE("material card cache: the read-side hash IS the scan's hash, across a chunk boundary (CC14)") {
    // ANTI-VACUITY for CC11-CC13: the scan hashes a file STREAMING, in HASH_CHUNK_BYTES reads through
    // ContentHasher, while the cache hashes the bytes it read in ONE shot. A valid document padded past one
    // chunk with trailing whitespace makes the two disagree if either hasher ever differed -- and a cache
    // whose check could never match would store no card for ANY material, which CC1 alone would not explain.
    std::string text = materialText("Studio Brass", 0.5F);
    text.append(engine::editor::HASH_CHUNK_BYTES + 4097U, ' ');
    const std::unique_ptr<CardProject> project = makeCardProject({{"brass.aeromat", text}});
    const ThumbnailKey key = keyOf(*project, "brass.aeromat");
    const engine::editor::FileBytesResult file = engine::editor::readFileBytes(
        project->assetsRoot + "/brass.aeromat", engine::editor::MAX_THUMBNAIL_SOURCE_BYTES);
    REQUIRE(file.bytes.has_value());
    REQUIRE(file.bytes->size() > engine::editor::HASH_CHUNK_BYTES);
    const engine::ContentHash readSide =
        engine::hashBytes(std::as_bytes(std::span<const char>(file.bytes->data(), file.bytes->size())));
    CHECK((readSide == key.hash));
    MaterialCardCache cache;
    cache.noteCardWanted(key);
    cache.service(project->database, 1U);
    CHECK(cache.readCount() == 1U);
    const MaterialCard* const card = cache.cardFor(key);
    REQUIRE(card != nullptr);  // the check is LIVE and MATCHES: an unchanged file still gets its card
    CHECK(card->displayName == "Studio Brass");
}
