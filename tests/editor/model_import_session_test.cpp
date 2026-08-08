// tests/editor/model_import_session_test.cpp -- task 3.2.1: ModelImportSession, the on-demand,
// two-pass model import driver. A TU of aero_editor_shell_test, which supplies main() from
// shell_test.cpp -- do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// UNGATED, tier-0 (D4/AC-17/INV-P5, the model_import_test.cpp precedent): model_import_session.hpp
// reaches nothing gated on AERO_REFLECT_TOOLS (only engine/scene_serialize is), so every case here must
// be PRESENT and PASSING in all three build configurations. Every case is driven from a string literal
// against a real, scratch TempDir -- ModelImportSession genuinely reads files (through text_file.hpp),
// unlike model_import_test.cpp's pure importModel() cases.
#include <aero/core/guid.hpp>
#include <aero/editor/asset_database.hpp>
#include <aero/editor/model_import_session.hpp>

#include "scene_golden_support.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace {

// The SEVENTH+ TU-local copy of this shape (asset_database_test.cpp:53's precedent; scaffolding is
// copied, the ASSERTION is shared).
class TempDir {
public:
    TempDir() {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        static int counter = 0;  // doctest runs serially in one process; a plain counter is unique enough
        dirPath = base / ("aero_model_import_session_test_" + std::to_string(++counter));
        std::filesystem::remove_all(dirPath, ec);
        std::filesystem::create_directories(dirPath, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(dirPath, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&) = delete;
    TempDir& operator=(TempDir&&) = delete;

    [[nodiscard]] std::string utf8() const {
        const std::u8string bytes = dirPath.u8string();
        return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }
    [[nodiscard]] std::string join(std::string_view leaf) const {
        std::string result = utf8();
        result += '/';
        result += leaf;
        return result;
    }

private:
    std::filesystem::path dirPath;
};

// UTF-8 bytes, never a narrow std::string (project_files.cpp's pathFromUtf8 precedent,
// asset_database_test.cpp:95's TU-local copy).
[[nodiscard]] std::filesystem::path pathOf(std::string_view utf8) {
    const std::u8string bytes(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(bytes);
}

void writeFile(std::string_view absolutePath, std::string_view bytes) {
    std::ofstream out(pathOf(absolutePath), std::ios::binary | std::ios::trunc);
    REQUIRE(static_cast<bool>(out));
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

using engine::Guid;
using engine::GuidGenerator;
using engine::editor::AssetDatabase;
using engine::editor::AssetScanReport;
using engine::editor::ATOMIC_TEMP_SUFFIX;
using engine::editor::ImportSettings;
using engine::editor::ImportStatus;
using engine::editor::MAX_MODEL_FILE_BYTES;
using engine::editor::ModelImportSession;
using engine::editor::ScanStatus;
using engine::editor::SessionState;
using engine::editor::writeMetaText;

// ---- MS1-MS7: the reconcile surface, Idle/NotImportable, AC-45/AC-46/AC-47 -------------------------

TEST_CASE("model_import_session: a fresh session is Idle with importCount() == 0 (MS1)") {
    const ModelImportSession session;
    CHECK(session.state() == SessionState::Idle);
    CHECK(session.importCount() == 0);
    CHECK(session.target().empty());
    CHECK(session.generation() == 0);
}

TEST_CASE("model_import_session: setTarget then one service() imports exactly once (MS2, AC-45)") {
    const TempDir dir;
    writeFile(dir.join("a.gltf"), R"({"asset":{"version":"2.0"}})");
    AssetDatabase db;
    GuidGenerator gen(2);
    db.rescan(dir.utf8(), dir.utf8(), gen);

    ModelImportSession session;
    session.setTarget("a.gltf", db.generation());
    session.service(dir.utf8(), db);
    CHECK(session.state() == SessionState::Imported);
    CHECK(session.importCount() == 1);
    CHECK(session.result().status == ImportStatus::Ok);
}

TEST_CASE("model_import_session: ten further service() calls leave importCount() at 1 (MS3, AC-45, structural)") {
    const TempDir dir;
    writeFile(dir.join("a.gltf"), R"({"asset":{"version":"2.0"}})");
    AssetDatabase db;
    GuidGenerator gen(3);
    db.rescan(dir.utf8(), dir.utf8(), gen);

    ModelImportSession session;
    session.setTarget("a.gltf", db.generation());
    session.service(dir.utf8(), db);
    REQUIRE(session.importCount() == 1);
    for (int i = 0; i < 10; ++i) {
        session.service(dir.utf8(), db);
    }
    CHECK(session.importCount() == 1);
}

TEST_CASE("model_import_session: setTarget with the SAME target and generation is a no-op (MS4, E18)") {
    const TempDir dir;
    writeFile(dir.join("a.gltf"), R"({"asset":{"version":"2.0"}})");
    AssetDatabase db;
    GuidGenerator gen(4);
    db.rescan(dir.utf8(), dir.utf8(), gen);

    ModelImportSession session;
    session.setTarget("a.gltf", db.generation());
    session.service(dir.utf8(), db);
    REQUIRE(session.importCount() == 1);

    session.setTarget("a.gltf", db.generation());  // SAME target, SAME generation -- a documented no-op
    session.service(dir.utf8(), db);
    CHECK(session.importCount() == 1);
}

TEST_CASE("model_import_session: a NEW generation re-imports exactly once (MS5, AC-47)") {
    const TempDir dir;
    writeFile(dir.join("a.gltf"), R"({"asset":{"version":"2.0"}})");
    AssetDatabase db;
    GuidGenerator gen(5);
    db.rescan(dir.utf8(), dir.utf8(), gen);

    ModelImportSession session;
    session.setTarget("a.gltf", db.generation());
    session.service(dir.utf8(), db);
    REQUIRE(session.importCount() == 1);

    session.setTarget("a.gltf", db.generation() + 1);  // simulates a rescan bumping generation()
    session.service(dir.utf8(), db);
    CHECK(session.importCount() == 2);
    session.service(dir.utf8(), db);  // a further tick at the SAME generation costs nothing more
    CHECK(session.importCount() == 2);
}

TEST_CASE("model_import_session: an empty target is Idle and imports nothing (MS6, AC-46)") {
    const AssetDatabase db;  // never scanned -- a garbage root below proves nothing was read either
    ModelImportSession session;
    session.service("/this/path/must/never/be/opened", db);
    CHECK(session.state() == SessionState::Idle);
    CHECK(session.importCount() == 0);
}

TEST_CASE("model_import_session: a non-model target is NotImportable and imports nothing (MS7, AC-46)") {
    const AssetDatabase db;  // never scanned
    ModelImportSession session;
    session.setTarget("notes.txt", 0);
    session.service("/this/path/must/never/be/opened", db);
    CHECK(session.state() == SessionState::NotImportable);
    CHECK(session.importCount() == 0);
}

// ---- MS8-MS10: the two-pass drive over real bytes -----------------------------------------------

TEST_CASE("model_import_session: a model whose external buffer exists imports Full with vectors filled (MS8)") {
    const TempDir dir;
    writeFile(dir.join("a.gltf"),
              R"({"asset":{"version":"2.0"},"meshes":[{"primitives":[{"attributes":{"POSITION":0},"mode":4}]}],)"
              R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}],)"
              R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],)"
              R"("buffers":[{"byteLength":36,"uri":"a.bin"}]})");
    writeFile(dir.join("a.bin"), std::string(36, '\0'));
    AssetDatabase db;
    GuidGenerator gen(8);
    db.rescan(dir.utf8(), dir.utf8(), gen);

    ModelImportSession session;
    session.setTarget("a.gltf", db.generation());
    session.service(dir.utf8(), db);
    CHECK(session.state() == SessionState::Imported);
    CHECK(session.result().status == ImportStatus::Ok);
    REQUIRE(session.result().model.meshes.size() == 1);
    REQUIRE(session.result().model.meshes[0].primitives.size() == 1);
    CHECK(session.result().model.meshes[0].primitives[0].positions.size() == 3);
    CHECK(session.result().model.meshes[0].primitives[0].indices.size() == 3);  // synthesized (AC-23)
}

TEST_CASE(
    "model_import_session: a missing external buffer imports as Failed/MissingBuffer with a message "
    "(MS9)") {
    const TempDir dir;
    writeFile(dir.join("a.gltf"),
              R"({"asset":{"version":"2.0"},"meshes":[{"primitives":[{"attributes":{"POSITION":0},"mode":4}]}],)"
              R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}],)"
              R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],)"
              R"("buffers":[{"byteLength":36,"uri":"missing.bin"}]})");
    // Deliberately NO missing.bin on disk -- pass 1 names it, pass 2 cannot load it.
    AssetDatabase db;
    GuidGenerator gen(9);
    db.rescan(dir.utf8(), dir.utf8(), gen);

    ModelImportSession session;
    session.setTarget("a.gltf", db.generation());
    session.service(dir.utf8(), db);
    CHECK(session.state() == SessionState::Failed);
    CHECK(session.result().status == ImportStatus::MissingBuffer);
    CHECK_FALSE(session.result().message.empty());
}

TEST_CASE(
    "model_import_session: a target deleted between the reconcile and service() fails with the "
    "OS reason (MS10, E15)") {
    const TempDir dir;
    writeFile(dir.join("a.gltf"), R"({"asset":{"version":"2.0"}})");
    AssetDatabase db;
    GuidGenerator gen(10);
    db.rescan(dir.utf8(), dir.utf8(), gen);

    ModelImportSession session;
    session.setTarget("a.gltf", db.generation());

    std::error_code ec;
    std::filesystem::remove(pathOf(dir.join("a.gltf")), ec);
    REQUIRE_FALSE(ec);

    session.service(dir.utf8(), db);
    CHECK(session.state() == SessionState::Failed);
    CHECK(session.result().status == ImportStatus::ParseFailed);
    CHECK_FALSE(session.result().message.empty());
}

// ---- MS11-MS15: the settings form and the ONE write this task adds (AC-51/AC-52) -------------------

TEST_CASE(
    "model_import_session: applySettings writes the sidecar atomically, no .aero-tmp left "
    "behind (MS11, AC-51)") {
    const TempDir dir;
    writeFile(dir.join("a.gltf"), R"({"asset":{"version":"2.0"}})");
    AssetDatabase db;
    GuidGenerator gen(11);
    db.rescan(dir.utf8(), dir.utf8(), gen);
    const Guid guid = *db.guidForPath("a.gltf");

    ModelImportSession session;
    session.setTarget("a.gltf", db.generation());
    session.service(dir.utf8(), db);
    REQUIRE(session.state() == SessionState::Imported);

    ImportSettings edited;
    edited.scale = 2.5F;
    session.setPendingSettings(edited);
    REQUIRE(session.canApply());
    const std::string error = session.applySettings(dir.utf8());
    CHECK(error.empty());
    CHECK(session.applyError().empty());

    const auto metaText = scene_golden::readBytes(dir.join("a.gltf.meta"));
    REQUIRE(metaText.ok);
    CHECK(metaText.text == writeMetaText(guid, edited));
    CHECK_FALSE(std::filesystem::exists(pathOf(dir.join("a.gltf.meta") + std::string(ATOMIC_TEMP_SUFFIX))));
}

TEST_CASE("model_import_session: settingsDirty() clears after Apply and re-arms on a new edit (MS12)") {
    const TempDir dir;
    writeFile(dir.join("a.gltf"), R"({"asset":{"version":"2.0"}})");
    AssetDatabase db;
    GuidGenerator gen(12);
    db.rescan(dir.utf8(), dir.utf8(), gen);

    ModelImportSession session;
    session.setTarget("a.gltf", db.generation());
    session.service(dir.utf8(), db);
    REQUIRE(session.state() == SessionState::Imported);
    CHECK_FALSE(session.settingsDirty());  // fresh from disk -- pending == onDisk by construction

    ImportSettings edited = session.pendingSettings();
    edited.scale = 3.0F;
    session.setPendingSettings(edited);
    CHECK(session.settingsDirty());

    const std::string error = session.applySettings(dir.utf8());
    REQUIRE(error.empty());
    CHECK_FALSE(session.settingsDirty());
}

TEST_CASE("model_import_session: revertSettings() restores pending from onDisk (MS13)") {
    const TempDir dir;
    writeFile(dir.join("a.gltf"), R"({"asset":{"version":"2.0"}})");
    AssetDatabase db;
    GuidGenerator gen(13);
    db.rescan(dir.utf8(), dir.utf8(), gen);

    ModelImportSession session;
    session.setTarget("a.gltf", db.generation());
    session.service(dir.utf8(), db);
    REQUIRE(session.state() == SessionState::Imported);
    const ImportSettings original = session.pendingSettings();

    ImportSettings edited = original;
    edited.scale = 9.0F;
    session.setPendingSettings(edited);
    REQUIRE(session.settingsDirty());

    session.revertSettings();
    CHECK_FALSE(session.settingsDirty());
    CHECK(session.pendingSettings() == original);
}

TEST_CASE(
    "model_import_session: an invalid .meta identity disables Apply and writes nothing (MS14, E16, "
    "seed S21)") {
    const TempDir dir;
    writeFile(dir.join("a.gltf"), R"({"asset":{"version":"2.0"}})");
    // A nil GUID -- MetaError::NilGuid -- makes the record AssetMetaState::Invalid (D7: never repaired).
    const std::string invalidMeta = "{\n  \"version\": 1,\n  \"guid\": \"" + std::string(32, '0') + "\"\n}\n";
    writeFile(dir.join("a.gltf.meta"), invalidMeta);
    AssetDatabase db;
    GuidGenerator gen(14);
    const AssetScanReport report = db.rescan(dir.utf8(), dir.utf8(), gen);
    REQUIRE(report.status == ScanStatus::Ok);
    REQUIRE(report.invalid == 1);  // confirms the seed landed: this really IS an Invalid record

    ModelImportSession session;
    session.setTarget("a.gltf", db.generation());
    session.service(dir.utf8(), db);
    CHECK(session.state() == SessionState::Imported);  // import needs BYTES, not identity (E16)
    CHECK_FALSE(session.canApply());

    const auto before = scene_golden::readBytes(dir.join("a.gltf.meta"));
    REQUIRE(before.ok);
    const std::string error = session.applySettings(dir.utf8());
    CHECK_FALSE(error.empty());
    const auto after = scene_golden::readBytes(dir.join("a.gltf.meta"));
    REQUIRE(after.ok);
    CHECK(after.text == before.text);  // NOTHING was written -- the invalid sidecar is untouched
}

TEST_CASE("model_import_session: a write failure changes nothing on disk (MS15, POSIX)") {
    const TempDir dir;
    writeFile(dir.join("a.gltf"), R"({"asset":{"version":"2.0"}})");
    AssetDatabase db;
    GuidGenerator gen(15);
    db.rescan(dir.utf8(), dir.utf8(), gen);

    ModelImportSession session;
    session.setTarget("a.gltf", db.generation());
    session.service(dir.utf8(), db);
    REQUIRE(session.state() == SessionState::Imported);

    const auto before = scene_golden::readBytes(dir.join("a.gltf.meta"));
    REQUIRE(before.ok);
    ImportSettings edited = session.pendingSettings();
    edited.scale = 4.0F;
    session.setPendingSettings(edited);

    std::error_code ec;
    std::filesystem::permissions(pathOf(dir.utf8()),
                                 std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::replace, ec);
    REQUIRE_FALSE(ec);

    const std::string error = session.applySettings(dir.utf8());

    std::error_code restoreEc;
    std::filesystem::permissions(pathOf(dir.utf8()), std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, restoreEc);

    if (error.empty()) {
        MESSAGE("running as a user for whom a read-only directory does not block file creation -- seed did not land");
        return;
    }
    CHECK_FALSE(error.empty());
    CHECK_FALSE(session.applyError().empty());
    CHECK(session.settingsDirty());  // onDisk unchanged -- Apply never disabled itself

    const auto after = scene_golden::readBytes(dir.join("a.gltf.meta"));
    REQUIRE(after.ok);
    CHECK(after.text == before.text);  // the file on disk is UNCHANGED
}

// ---- MS16: the whole-file cap, refused without being opened (AC-43/E20) ----------------------------

TEST_CASE(
    "model_import_session: a model over MAX_MODEL_FILE_BYTES fails without being opened; the observed "
    "size is kept (MS16, AC-43, E20)") {
    const TempDir dir;
    writeFile(dir.join("a.gltf"), "");  // create it empty, then SPARSELY extend it -- no real I/O
    std::error_code ec;
    std::filesystem::resize_file(pathOf(dir.join("a.gltf")), MAX_MODEL_FILE_BYTES + 1, ec);
    REQUIRE_FALSE(ec);

    const AssetDatabase db;  // never scanned -- service() needs no record to attempt this read
    ModelImportSession session;
    session.setTarget("a.gltf", 0);
    session.service(dir.utf8(), db);
    CHECK(session.state() == SessionState::Failed);
    CHECK(session.fileSizeBytes() == MAX_MODEL_FILE_BYTES + 1);
}
