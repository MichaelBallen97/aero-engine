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
#include <aero/editor/text_file.hpp>  // task 3.2.2, MS22: readTextFile, for the source-text proof

#include "scene_golden_support.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

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

// ---- task 3.2.2: a minimal ASCII FBX document, the §D-7/§G-10 template's shape, VERIFIED TO PARSE
// (fbx_import_test.cpp's own spike). This TU drives the REAL session, through the REAL importFbx
// backend -- a second, independent copy of the template rather than a shared header, matching this
// file's own "each TU keeps its own" precedent (asset_database_test.cpp's identical copy, task 3.2.2).
[[nodiscard]] std::string makeFbxDoc(std::string_view extraObjects) {
    return std::string(
               "; FBX 7.4.0 project file\n"
               "FBXHeaderExtension:  {\n"
               "    FBXHeaderVersion: 1003\n"
               "    FBXVersion: 7400\n"
               "    Creator: \"aero test fixture\"\n"
               "}\n"
               "GlobalSettings:  {\n"
               "    Version: 1000\n"
               "    Properties70:  {\n"
               "        P: \"UpAxis\", \"int\", \"Integer\", \"\",1\n"
               "        P: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n"
               "        P: \"FrontAxis\", \"int\", \"Integer\", \"\",2\n"
               "        P: \"FrontAxisSign\", \"int\", \"Integer\", \"\",1\n"
               "        P: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n"
               "        P: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n"
               "        P: \"UnitScaleFactor\", \"double\", \"Number\", \"\",100\n"
               "    }\n"
               "}\n"
               "Objects:  {\n"
               "    Geometry: 200, \"Geometry::box\", \"Mesh\" {\n"
               "        Vertices: *12 { a: 0,0,0,1,0,0,1,1,0,0,1,0 }\n"
               "        PolygonVertexIndex: *4 { a: 0,1,2,-4 }\n"
               "        GeometryVersion: 124\n"
               "    }\n"
               "    Model: 100, \"Model::box\", \"Mesh\" { Version: 232 }\n") +
           std::string(extraObjects) +
           "}\n"
           "Connections:  {\n"
           "    C: \"OO\",100,0\n"
           "    C: \"OO\",200,100\n"
           "}\n";
}

// A standalone Texture OBJECT, connected to nothing -- fbx_import.cpp's phase 4 walks `scene.textures`
// directly (every element in the document), the shape fbx_import_test.cpp's FI50/FI51/FI52 rely on.
[[nodiscard]] std::string fbxTextureObject(std::string_view relativeFilename) {
    return "    Texture: 500, \"Texture::tex\", \"\" { Type: \"TextureVideoClip\" Version: 202 "
           "RelativeFilename: \"" +
           std::string(relativeFilename) + "\" }\n";
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
using engine::editor::readTextFile;
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
    "model_import_session: a model in a SUBDIRECTORY whose external buffer sits beside it imports "
    "Full with vectors filled (MS8b, code-review BLOCKING-1)") {
    // BLOCKING-1: the glTF document's raw URI ("chair.bin") is NOT what ExternalBuffer::uri holds --
    // that field is classifyUri's RESOLVED, project-relative path ("models/chair.bin"), exactly what
    // this same session already read from structure.externalUris a few lines above (model_import_
    // session.cpp). MS8 alone cannot catch a comparison between the two forms disagreeing, because it
    // puts both files at the assets ROOT, where assetRelativeDir is "" and the raw and resolved forms
    // are byte-identical by coincidence.
    const TempDir dir;
    std::error_code ec;
    std::filesystem::create_directories(pathOf(dir.join("models")), ec);
    REQUIRE_FALSE(ec);
    writeFile(dir.join("models/chair.gltf"),
              R"({"asset":{"version":"2.0"},"meshes":[{"primitives":[{"attributes":{"POSITION":0},"mode":4}]}],)"
              R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}],)"
              R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],)"
              R"("buffers":[{"byteLength":36,"uri":"chair.bin"}]})");
    writeFile(dir.join("models/chair.bin"), std::string(36, '\0'));
    AssetDatabase db;
    GuidGenerator gen(108);
    db.rescan(dir.utf8(), dir.utf8(), gen);

    ModelImportSession session;
    session.setTarget("models/chair.gltf", db.generation());
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

TEST_CASE(
    "model_import_session: a failed Apply leaves the ORIGINAL sidecar byte-identical -- the atomic "
    "write's only real discriminator (MS15b, AC-51, INV-M9, seed S22)") {
    // THE GAP THIS CLOSES, stated plainly. Seed S22 (replacing applySettings' writeTextFileAtomic with
    // a raw std::ofstream) left the WHOLE suite green -- 990/990, 83/83, six guards. MS11 cannot see it:
    // it asserts the `.aero-tmp` companion is ABSENT after a SUCCESSFUL apply, which a non-atomic write
    // satisfies trivially, having never created one. Check B of check-project-no-delete.sh cannot see it
    // either -- an ofstream is neither a remove nor a rename.
    //
    // The property that actually separates the two implementations is what happens when the write
    // FAILS: `std::ofstream(target, trunc)` zeroes the file BEFORE it can fail on anything downstream,
    // so a crash or a full disk costs the asset its identity permanently; the atomic path writes a
    // TEMP and renames, so the original survives untouched. That is the whole reason INV-M9 exists.
    //
    // The failure is constructed deterministically and portably by making the temp path UNUSABLE while
    // leaving the target perfectly writable: a DIRECTORY named exactly `<sidecar>.aero-tmp`. Opening a
    // directory for writing fails on all three OSes, so writeTextFileAtomic returns at its first
    // branch, having touched nothing. Deliberately NOT the chmod trick MS15 uses -- that one depends on
    // the running user (MS15 carries an explicit escape hatch for a user root can defeat), and an
    // escape hatch is exactly where a seed hides.
    const TempDir dir;
    writeFile(dir.join("a.gltf"), R"({"asset":{"version":"2.0"}})");
    AssetDatabase db;
    GuidGenerator gen(122);
    db.rescan(dir.utf8(), dir.utf8(), gen);

    ModelImportSession session;
    session.setTarget("a.gltf", db.generation());
    session.service(dir.utf8(), db);
    REQUIRE(session.state() == SessionState::Imported);

    const auto before = scene_golden::readBytes(dir.join("a.gltf.meta"));
    REQUIRE(before.ok);
    REQUIRE_FALSE(before.text.empty());  // there IS something to lose

    const std::string blocker = dir.join("a.gltf.meta") + std::string(ATOMIC_TEMP_SUFFIX);
    std::error_code ec;
    REQUIRE(std::filesystem::create_directory(pathOf(blocker), ec));
    REQUIRE_FALSE(ec);

    ImportSettings edited = session.pendingSettings();
    edited.scale = 5.0F;
    session.setPendingSettings(edited);
    REQUIRE(session.canApply());

    const std::string error = session.applySettings(dir.utf8());

    // Under seed S22 all four of these invert: a raw ofstream opens the target happily, truncates it,
    // writes the new text and reports SUCCESS.
    CHECK_FALSE(error.empty());
    CHECK_FALSE(session.applyError().empty());
    CHECK(session.settingsDirty());  // onDisk never advanced -- Apply stays armed
    const auto after = scene_golden::readBytes(dir.join("a.gltf.meta"));
    REQUIRE(after.ok);
    CHECK(after.text == before.text);  // BYTE-IDENTICAL: the identity survived the failure
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

// ---- MS17-MS20: code-review SHOULD-FIX 4/5 -- the reconcile window between setTarget() and
// service(), and applySettings()'s own guard ----------------------------------------------------------

TEST_CASE(
    "model_import_session: a target with no AssetDatabase record imports at DEFAULT settings, never "
    "the previously selected asset's (MS17, code review SHOULD-FIX 4)") {
    const TempDir dir;
    writeFile(dir.join("a.gltf"), R"({"asset":{"version":"2.0"}})");
    AssetDatabase db;
    GuidGenerator gen(117);
    db.rescan(dir.utf8(), dir.utf8(), gen);  // "c.gltf" does not exist yet -- the scan never sees it

    ModelImportSession session;
    session.setTarget("a.gltf", db.generation());
    session.service(dir.utf8(), db);
    REQUIRE(session.state() == SessionState::Imported);
    ImportSettings edited = session.pendingSettings();
    edited.scale = 6.0F;
    session.setPendingSettings(edited);
    REQUIRE(session.settingsDirty());

    // "c.gltf" exists on disk (so the import itself can succeed) but the database was never rescanned
    // after it was written, so findByPath() returns nullptr for it -- a genuinely RECORDLESS target.
    writeFile(dir.join("c.gltf"), R"({"asset":{"version":"2.0"}})");
    session.setTarget("c.gltf", db.generation());
    session.service(dir.utf8(), db);

    CHECK(session.state() == SessionState::Imported);
    // BEFORE this fix: pending/onDisk still held "a.gltf"'s edited scale (6.0) here -- an unrecorded
    // model imported at the PREVIOUSLY selected model's scale and flags.
    CHECK(session.pendingSettings() == ImportSettings{});
    CHECK(session.diskSettings() == ImportSettings{});
    CHECK_FALSE(session.canApply());  // no identity either -- E16
}

TEST_CASE(
    "model_import_session: an unrelated generation bump does not clobber an unapplied edit on the SAME "
    "target (MS18, code review SHOULD-FIX 4)") {
    const TempDir dir;
    writeFile(dir.join("a.gltf"), R"({"asset":{"version":"2.0"}})");
    AssetDatabase db;
    GuidGenerator gen(118);
    db.rescan(dir.utf8(), dir.utf8(), gen);

    ModelImportSession session;
    session.setTarget("a.gltf", db.generation());
    session.service(dir.utf8(), db);
    REQUIRE(session.state() == SessionState::Imported);

    ImportSettings edited = session.pendingSettings();
    edited.scale = 7.0F;
    session.setPendingSettings(edited);
    REQUIRE(session.settingsDirty());

    // Simulate an UNRELATED file's rescan bumping generation() while the SAME target stays selected --
    // exactly what editor_app.cpp's reconcile drives from an AssetWatcher-triggered rescan (the target
    // path is unchanged; only the generation moves).
    session.setTarget("a.gltf", db.generation() + 1);
    session.service(dir.utf8(), db);

    // BEFORE this fix: service() unconditionally re-read onDisk from the (unchanged) record and reset
    // pending = onDisk, silently discarding the edit within the same tick.
    CHECK(session.settingsDirty());
    CHECK(session.pendingSettings().scale == 7.0F);
}

TEST_CASE(
    "model_import_session: an Apply landing between setTarget() and service() refuses instead of "
    "writing the PREVIOUS target's GUID into the NEW target's sidecar (MS19, code review SHOULD-FIX 5)") {
    const TempDir dir;
    writeFile(dir.join("a.gltf"), R"({"asset":{"version":"2.0"}})");
    writeFile(dir.join("b.gltf"), R"({"asset":{"version":"2.0"}})");
    AssetDatabase db;
    GuidGenerator gen(119);
    db.rescan(dir.utf8(), dir.utf8(), gen);

    ModelImportSession session;
    session.setTarget("a.gltf", db.generation());
    session.service(dir.utf8(), db);
    REQUIRE(session.state() == SessionState::Imported);
    REQUIRE_FALSE(session.canApply());  // fresh from disk, not dirty yet

    const auto beforeB = scene_golden::readBytes(dir.join("b.gltf.meta"));
    REQUIRE(beforeB.ok);

    // editor_app.cpp's REAL ordering: the reconcile calls setTarget() for the new selection (:589)
    // BEFORE the Apply drain runs (:596-621); service() for "b.gltf" (:692, post-draw) has not run yet.
    session.setTarget("b.gltf", db.generation());
    ImportSettings edited;
    edited.scale = 9.0F;
    session.setPendingSettings(edited);
    // BEFORE this fix: targetGuid still validly named "a.gltf" here, so canApply() was TRUE.
    CHECK_FALSE(session.canApply());

    const std::string error = session.applySettings(dir.utf8());
    CHECK_FALSE(error.empty());

    const auto afterB = scene_golden::readBytes(dir.join("b.gltf.meta"));
    REQUIRE(afterB.ok);
    CHECK(afterB.text == beforeB.text);  // "b.gltf.meta" is UNCHANGED -- never received "a.gltf"'s guid
}

TEST_CASE(
    "model_import_session: applySettings() no-ops without touching the sidecar when settings are not "
    "dirty (MS20, code review SHOULD-FIX 5)") {
    const TempDir dir;
    writeFile(dir.join("a.gltf"), R"({"asset":{"version":"2.0"}})");
    AssetDatabase db;
    GuidGenerator gen(120);
    db.rescan(dir.utf8(), dir.utf8(), gen);

    ModelImportSession session;
    session.setTarget("a.gltf", db.generation());
    session.service(dir.utf8(), db);
    REQUIRE(session.state() == SessionState::Imported);
    REQUIRE_FALSE(session.settingsDirty());  // fresh from disk

    std::error_code ec;
    const std::filesystem::file_time_type beforeMtime =
        std::filesystem::last_write_time(pathOf(dir.join("a.gltf.meta")), ec);
    REQUIRE_FALSE(ec);

    // A hook-driven Apply (EditorApp::requestModelImportApply(), which bypasses the panel's own
    // BeginDisabled(!canApply())) with nothing to save. BEFORE this fix, applySettings() checked only
    // targetGuid.valid() and rewrote a byte-identical sidecar anyway -- dirtying its mtime for nothing.
    const std::string error = session.applySettings(dir.utf8());
    CHECK(error.empty());  // not a failure -- nothing needed writing

    const std::filesystem::file_time_type afterMtime =
        std::filesystem::last_write_time(pathOf(dir.join("a.gltf.meta")), ec);
    REQUIRE_FALSE(ec);
    CHECK(afterMtime == beforeMtime);  // the file was never even opened for writing
}

// ---- MS21: NIT 11 -- ImportedImage::guid ------------------------------------------------------------

TEST_CASE(
    "model_import_session: a resolved image URI's guid is populated from the database; an unresolved "
    "one stays nil (MS21, NIT 11)") {
    const TempDir dir;
    writeFile(dir.join("tex.png"), std::string(16, '\0'));  // any bytes -- the database never decodes it
    writeFile(dir.join("a.gltf"), R"({"asset":{"version":"2.0"},)"
                                  R"("images":[{"uri":"tex.png"},{"uri":"http://evil/x.png"}]})");
    AssetDatabase db;
    GuidGenerator gen(121);
    db.rescan(dir.utf8(), dir.utf8(), gen);
    const Guid textureGuid = *db.guidForPath("tex.png");
    REQUIRE(textureGuid.valid());

    ModelImportSession session;
    session.setTarget("a.gltf", db.generation());
    session.service(dir.utf8(), db);
    CHECK(session.state() == SessionState::Imported);
    REQUIRE(session.result().model.images.size() == 2);
    CHECK(session.result().model.images[0].relativePath == "tex.png");
    CHECK(session.result().model.images[0].guid == textureGuid);
    CHECK(session.result().model.images[1].relativePath.empty());  // refused scheme -- E7
    CHECK_FALSE(session.result().model.images[1].guid.valid());    // stays nil
}

// ---- MS22-MS27: task 3.2.2's D5 gate -- FBX skips the Structure pass entirely --------------------
//
// Renumbered from the plan's own predicted MS24-MS29: `/usr/bin/grep -c '^TEST_CASE('` on this file
// reads 23 BEFORE this block, but the highest NUMBERED case is MS21 (MS8b/MS15b are lettered variants
// of earlier numbers, not new ones) -- measured, not assumed, exactly the AD-i11/AD-i12/AD-i13
// collision this task's own asset_database_test.cpp block already logs for the identical reason.

TEST_CASE(
    "model_import_session: service()'s ONLY unconditional readFileBytes() call is the model's own "
    "bytes -- the external-buffer loop sits ENTIRELY inside the modelImporterNeedsExternalBuffers gate "
    "(MS22, AC-56b, the session-level gate-shape discriminator)") {
    // WHY THIS IS A SOURCE-TEXT PROOF, not a behavioural one (I60's own precedent, restated): FBX's
    // `importFbx` takes `external` ONLY to keep its signature uniform with importGltf's and NEVER reads
    // it (fbx_import.hpp's own header comment, D5) -- so whether the external-buffer loop ran or not is
    // UNOBSERVABLE in ImportResult for an ordinary fixture. A missing texture reference is swallowed
    // identically either way (readFileBytes fails -> `continue` -> `externals` stays empty regardless),
    // and the only path where running the loop WOULD change the outcome (E21's overBudget branch)
    // needs a cumulative external-buffer size near MAX_EXTERNAL_BYTES_PER_MODEL (512 MiB) to trip --
    // flatly incompatible with a tier-0 fixture. So the mechanical proof available is textual, the
    // identical shape I60 already uses for INV-M12: read model_import_session.cpp's own source,
    // strip comments, and prove the SHAPE rather than guess at a side effect no fixture can produce.
    //
    // WHAT THIS DOES NOT COVER: seed S29 itself (modelImporterNeedsExternalBuffers returning TRUE for
    // .fbx) is a defect in model_import.cpp, not in this file's source text -- MI110
    // (model_import_test.cpp) is what discriminates THAT bug, at the pure-function level. This case
    // discriminates the COMPLEMENTARY defect: the session ignoring, misplacing or inverting a CORRECT
    // predicate's result. CONFIRMED DIRECTLY: replacing the gate's condition with a literal `true`
    // reddens this case (REQUIRE(gateOpenLine != code.size()) fails) while leaving MS23/MS24/MS25/MS26
    // fully green -- the missing-texture and refused-URI scenarios cannot see this class of bug at all
    // (§D-11's own comment states why), which is the reason this case exists as a source-text proof
    // rather than a content-based one.
    constexpr std::string_view SOURCE_PATH = AERO_EDITOR_SRC_DIR "/model_import_session.cpp";
    const engine::editor::FileReadResult read = readTextFile(SOURCE_PATH);
    REQUIRE(read.text.has_value());
    const std::string& text = *read.text;
    REQUIRE_FALSE(text.empty());

    std::vector<std::string_view> lines;
    std::string_view remaining = text;
    while (true) {
        const std::size_t newline = remaining.find('\n');
        if (newline == std::string_view::npos) {
            lines.push_back(remaining);
            break;
        }
        lines.push_back(remaining.substr(0, newline));
        remaining.remove_prefix(newline + 1U);
    }

    // Comment-stripped BEFORE matching (I42's lesson, I60's own precedent): this task's own prose names
    // `readFileBytes`/`modelImporterNeedsExternalBuffers` in comments far more than once.
    std::vector<std::string> code;
    code.reserve(lines.size());
    for (const std::string_view line : lines) {
        const std::size_t commentStart = line.find("//");
        code.emplace_back(commentStart == std::string_view::npos ? line : line.substr(0, commentStart));
    }

    std::size_t gateOpenLine = code.size();
    for (std::size_t i = 0; i < code.size(); ++i) {
        if (code[i].find("modelImporterNeedsExternalBuffers(leaf)") != std::string::npos) {
            REQUIRE(gateOpenLine == code.size());  // exactly ONE gate line in the whole file
            gateOpenLine = i;
        }
    }
    REQUIRE(gateOpenLine != code.size());  // the gate exists at all

    // Walk forward from the gate line, tracking brace depth, to find where its block CLOSES.
    int depth = 0;
    std::size_t gateCloseLine = code.size();
    for (std::size_t i = gateOpenLine; i < code.size(); ++i) {
        for (const char c : code[i]) {
            if (c == '{') {
                ++depth;
            } else if (c == '}') {
                --depth;
                if (depth == 0) {
                    gateCloseLine = i;
                    break;
                }
            }
        }
        if (gateCloseLine != code.size()) {
            break;
        }
    }
    REQUIRE(gateCloseLine != code.size());  // the gate's own block is well-formed and closes

    std::size_t readsOutsideGate = 0;
    std::size_t readsInsideGate = 0;
    for (std::size_t i = 0; i < code.size(); ++i) {
        if (code[i].find("readFileBytes(") == std::string::npos) {
            continue;
        }
        if (i >= gateOpenLine && i <= gateCloseLine) {
            ++readsInsideGate;
        } else {
            ++readsOutsideGate;
        }
    }
    CHECK(readsOutsideGate == 1);  // the model's OWN bytes -- read unconditionally, for every format
    CHECK(readsInsideGate == 1);   // the external-buffer loop's one read call -- gated, for glTF only

    // Pass 2 (the unconditional Full import) sits AFTER the gate closes -- so it runs regardless of
    // which arm the gate took, exactly the shape MS25/MS27 (below) exercise behaviourally.
    std::size_t pass2Line = code.size();
    for (std::size_t i = gateCloseLine; i < code.size(); ++i) {
        if (code[i].find("ImportDepth::Full, externals") != std::string::npos) {
            pass2Line = i;
            break;
        }
    }
    CHECK(pass2Line > gateCloseLine);
}

TEST_CASE(
    "model_import_session: the glTF two-external-buffer path is RE-RUN, unbroken by the FBX gate -- one "
    "geometry buffer AND one texture both resolve (MS23)") {
    // MS8/MS8b each use exactly ONE external file -- this scenario is the genuinely NEW regression
    // proof the D5 gate needs: the externalUris LOOP must still iterate more than once for glTF.
    const TempDir dir;
    writeFile(dir.join("chair.gltf"),
              R"({"asset":{"version":"2.0"},"meshes":[{"primitives":[{"attributes":{"POSITION":0},"mode":4}]}],)"
              R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}],)"
              R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36}],)"
              R"("buffers":[{"byteLength":36,"uri":"chair.bin"}],)"
              R"("images":[{"uri":"wood.png"}]})");
    writeFile(dir.join("chair.bin"), std::string(36, '\0'));
    writeFile(dir.join("wood.png"), "pixels");
    AssetDatabase db;
    GuidGenerator gen(200);
    db.rescan(dir.utf8(), dir.utf8(), gen);
    const Guid woodGuid = *db.guidForPath("wood.png");
    REQUIRE(woodGuid.valid());

    ModelImportSession session;
    session.setTarget("chair.gltf", db.generation());
    session.service(dir.utf8(), db);
    CHECK(session.state() == SessionState::Imported);
    CHECK(session.result().status == ImportStatus::Ok);
    REQUIRE(session.result().model.meshes.size() == 1);
    REQUIRE(session.result().model.meshes[0].primitives.size() == 1);
    CHECK(session.result().model.meshes[0].primitives[0].positions.size() == 3);  // the .bin resolved
    REQUIRE(session.result().model.images.size() == 1);
    CHECK(session.result().model.images[0].relativePath == "wood.png");  // the texture ALSO resolved
    CHECK(session.result().model.images[0].guid == woodGuid);
}

TEST_CASE(
    "model_import_session: an .fbx naming TEN refused texture URIs imports through the REAL session "
    "with ten refusals and an empty externalUris -- AC-52's no-read policy holds end to end, not only "
    "at the pure importModel() level (MS24, AC-52's session-level half)") {
    // fbx_import_test.cpp's own FI52 already proves this at the pure importModel() level ("proven
    // structurally: importFbx's signature takes only bytes"). This is the SAME property exercised
    // through the actual disk-reading session path (TempDir + AssetDatabase::rescan +
    // ModelImportSession::service()), which is a genuinely different code path even though the
    // outcome matches.
    const TempDir dir;
    std::string objects;
    for (int i = 0; i < 10; ++i) {
        objects += std::format(
            "    Texture: {}, \"Texture::t{}\", \"\" {{ Type: \"TextureVideoClip\" Version: 202 "
            "RelativeFilename: \"/etc/secret{}.png\" }}\n",
            500 + i, i, i);
    }
    writeFile(dir.join("chair.fbx"), makeFbxDoc(objects));
    AssetDatabase db;
    GuidGenerator gen(201);
    db.rescan(dir.utf8(), dir.utf8(), gen);

    ModelImportSession session;
    session.setTarget("chair.fbx", db.generation());
    session.service(dir.utf8(), db);
    CHECK(session.state() == SessionState::Imported);
    CHECK(session.result().status == ImportStatus::Ok);
    CHECK(session.result().externalUris.empty());
    REQUIRE(session.result().model.images.size() == 10);
    for (const auto& image : session.result().model.images) {
        CHECK(image.relativePath.empty());
        CHECK_FALSE(image.refusal.empty());
    }
}

TEST_CASE(
    "model_import_session: an .fbx whose referenced texture is MISSING still imports its geometry "
    "successfully -- status Ok, non-empty geometry, one warning (MS25, AC-57)") {
    // THE E21 FALLBACK MUST NOT FIRE FOR FBX. Asserting status == Ok AND vertexCount > 0 is what a
    // Truncated-with-structure-only fallback (§A-4's zeroed counts) would fail -- a weaker case
    // asserting status == Ok alone would not discriminate a bug that silently fell back to Structure.
    const TempDir dir;
    writeFile(dir.join("chair.fbx"), makeFbxDoc(fbxTextureObject("missing.png")));
    // Deliberately NO missing.png on disk.
    AssetDatabase db;
    GuidGenerator gen(202);
    db.rescan(dir.utf8(), dir.utf8(), gen);

    ModelImportSession session;
    session.setTarget("chair.fbx", db.generation());
    session.service(dir.utf8(), db);
    CHECK(session.state() == SessionState::Imported);
    CHECK(session.result().status == ImportStatus::Ok);
    REQUIRE(session.result().model.meshes.size() == 1);
    REQUIRE(session.result().model.meshes[0].primitives.size() == 1);
    CHECK(session.result().model.meshes[0].primitives[0].positions.size() > 0);
    CHECK(session.result().model.summary.vertexCount > 0);
}

TEST_CASE(
    "model_import_session: switching .fbx -> .gltf -> .fbx replaces the WHOLE result each time -- no "
    "field carried across formats (MS26, E21 of the spec's §8)") {
    const TempDir dir;
    writeFile(dir.join("chair.fbx"), makeFbxDoc(""));
    writeFile(dir.join("table.gltf"), R"({"asset":{"version":"2.0"}})");
    AssetDatabase db;
    GuidGenerator gen(203);
    db.rescan(dir.utf8(), dir.utf8(), gen);

    ModelImportSession session;
    session.setTarget("chair.fbx", db.generation());
    session.service(dir.utf8(), db);
    REQUIRE(session.state() == SessionState::Imported);
    CHECK(session.result().model.sourceSpace.declared);  // FBX declares a space
    REQUIRE(session.result().model.meshes.size() == 1);

    session.setTarget("table.gltf", db.generation());
    session.service(dir.utf8(), db);
    REQUIRE(session.state() == SessionState::Imported);
    CHECK_FALSE(session.result().model.sourceSpace.declared);  // glTF declares NONE -- not the FBX's
    CHECK(session.result().model.meshes.empty());              // a minimal glTF document has no meshes

    session.setTarget("chair.fbx", db.generation());
    session.service(dir.utf8(), db);
    REQUIRE(session.state() == SessionState::Imported);
    CHECK(session.result().model.sourceSpace.declared);  // back to FBX -- never stuck at glTF's "false"
    REQUIRE(session.result().model.meshes.size() == 1);
    CHECK(session.importCount() == 3);
}

TEST_CASE(
    "model_import_session: applySettings on an .fbx writes the sidecar ONCE, atomically, with "
    "\"name\": \"fbx\" -- a byte-identical re-apply writes nothing (MS27, AC-17, INV-M9/INV-F12)") {
    const TempDir dir;
    writeFile(dir.join("chair.fbx"), makeFbxDoc(""));
    AssetDatabase db;
    GuidGenerator gen(204);
    db.rescan(dir.utf8(), dir.utf8(), gen);
    const Guid guid = *db.guidForPath("chair.fbx");

    ModelImportSession session;
    session.setTarget("chair.fbx", db.generation());
    session.service(dir.utf8(), db);
    REQUIRE(session.state() == SessionState::Imported);

    ImportSettings edited;
    edited.scale = 3.5F;
    session.setPendingSettings(edited);
    REQUIRE(session.canApply());
    const std::string error = session.applySettings(dir.utf8());
    CHECK(error.empty());

    const auto metaText = scene_golden::readBytes(dir.join("chair.fbx.meta"));
    REQUIRE(metaText.ok);
    CHECK(metaText.text == writeMetaText(guid, edited, "fbx", 1));
    CHECK(metaText.text.find(R"("name": "fbx")") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(pathOf(dir.join("chair.fbx.meta") + std::string(ATOMIC_TEMP_SUFFIX))));

    // A byte-identical re-apply (settings unchanged) writes NOTHING -- INV-M9/INV-F12: this task adds
    // no write anywhere beyond the one applySettings() already owned.
    std::error_code ec;
    const std::filesystem::file_time_type beforeMtime =
        std::filesystem::last_write_time(pathOf(dir.join("chair.fbx.meta")), ec);
    REQUIRE_FALSE(ec);
    session.setPendingSettings(edited);  // the SAME settings again
    const std::string secondError = session.applySettings(dir.utf8());
    CHECK(secondError.empty());
    const std::filesystem::file_time_type afterMtime =
        std::filesystem::last_write_time(pathOf(dir.join("chair.fbx.meta")), ec);
    REQUIRE_FALSE(ec);
    CHECK(afterMtime == beforeMtime);
}

// ---- MS28-MS33: task 3.2.3, the .obj/.mtl session integration -------------------------------------

TEST_CASE(
    "model_import_session: setTarget then one service() imports an .obj exactly once, and ten further "
    "service() calls leave importCount() at 1 (MS28, AC-61)") {
    const TempDir dir;
    writeFile(dir.join("chair.obj"), "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    AssetDatabase db;
    GuidGenerator gen(205);
    db.rescan(dir.utf8(), dir.utf8(), gen);

    ModelImportSession session;
    session.setTarget("chair.obj", db.generation());
    session.service(dir.utf8(), db);
    CHECK(session.state() == SessionState::Imported);
    REQUIRE(session.importCount() == 1);
    CHECK(session.result().status == ImportStatus::Ok);

    for (int i = 0; i < 10; ++i) {
        session.service(dir.utf8(), db);
    }
    CHECK(session.importCount() == 1);
}

TEST_CASE(
    "model_import_session: an .obj naming a .mtl that itself names a texture resolves BOTH through the "
    "REAL session -- materials populated, the texture's path and guid resolved (MS29, the two-pass .obj "
    "read)") {
    // WHY THIS PROVES ".obj reads exactly its .mtl, never a texture" WITHOUT new instrumentation: MS22
    // (above) proves, from model_import_session.cpp's own source text, that the external-buffer loop's
    // ONE call site sits entirely inside the modelImporterNeedsExternalBuffers(leaf) gate and runs once
    // per entry in Structure's externalUris -- a claim that holds for EVERY format, not only glTF/FBX.
    // model_import_test.cpp's MI123 proves modelImporterNeedsExternalBuffers("a.obj") == true, so the
    // gate IS taken; obj_import_test.cpp's OI2 proves an .obj's Structure-depth externalUris is EXACTLY
    // the mtllib path -- one entry, never a texture (D5: the Structure pass is a pure text scan of
    // mtllib operands only, it never looks inside the .mtl for map_Kd). Combined, the loop performs
    // EXACTLY one external read (chair.mtl) and structurally cannot attempt a texture read -- there is
    // no entry in externalUris to read one from. This case is the BEHAVIOURAL half: proving the .mtl's
    // bytes the loop DID read actually make it into the Full pass and resolve correctly end to end,
    // matching MS23's shape for glTF's own two-external-buffer path.
    const TempDir dir;
    writeFile(dir.join("chair.obj"), "mtllib chair.mtl\nusemtl Wood\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    writeFile(dir.join("chair.mtl"), "newmtl Wood\nKd 0.5 0.3 0.1\nmap_Kd wood.png\n");
    writeFile(dir.join("wood.png"), "pixels");
    AssetDatabase db;
    GuidGenerator gen(206);
    db.rescan(dir.utf8(), dir.utf8(), gen);
    const Guid woodGuid = *db.guidForPath("wood.png");
    REQUIRE(woodGuid.valid());

    ModelImportSession session;
    session.setTarget("chair.obj", db.generation());
    session.service(dir.utf8(), db);
    CHECK(session.state() == SessionState::Imported);
    CHECK(session.result().status == ImportStatus::Ok);
    REQUIRE(session.result().model.materials.size() == 1);
    CHECK(session.result().model.materials[0].name == "Wood");
    REQUIRE(session.result().model.meshes.size() == 1);
    REQUIRE(session.result().model.meshes[0].primitives.size() == 1);
    CHECK(session.result().model.meshes[0].primitives[0].materialIndex == 0);
    REQUIRE(session.result().model.images.size() == 1);
    CHECK(session.result().model.images[0].relativePath == "wood.png");  // the .mtl's own texture resolved
    CHECK(session.result().model.images[0].guid == woodGuid);
}

TEST_CASE(
    "model_import_session: a .mtl selected DIRECTLY imports through the REAL session with no .obj in "
    "the picture at all -- materials populated (MS30, section A-5's false arm)") {
    // WHY THIS PROVES "exactly one read" WITHOUT new instrumentation: MI123 proves
    // modelImporterNeedsExternalBuffers("a.mtl") == false at the pure-function level; MS22 (above)
    // proves the session's ENTIRE external-buffer loop sits inside that gate and does not run at all
    // when the gate is false. So selecting a .mtl directly leaves ONLY the one unconditional "read the
    // target's own bytes" call MS22 already counts -- there is no second call site to reach, gated or
    // not. This case is the BEHAVIOURAL half: the .mtl's OWN bytes (the only bytes ever read) must
    // still resolve to correct materials through the real, disk-reading session path.
    const TempDir dir;
    writeFile(dir.join("swatch.mtl"), "newmtl Wood\nKd 0.5 0.3 0.1\n");
    AssetDatabase db;
    GuidGenerator gen(207);
    db.rescan(dir.utf8(), dir.utf8(), gen);

    ModelImportSession session;
    session.setTarget("swatch.mtl", db.generation());
    session.service(dir.utf8(), db);
    CHECK(session.state() == SessionState::Imported);
    CHECK(session.result().status == ImportStatus::Ok);
    REQUIRE(session.result().model.materials.size() == 1);
    CHECK(session.result().model.materials[0].name == "Wood");
    CHECK(session.result().model.nodes.empty());  // AC-24: a .mtl carries no nodes/meshes
    CHECK(session.result().model.meshes.empty());
}

TEST_CASE(
    "model_import_session: an .obj whose .mtl is referenced but ABSENT on disk imports through the REAL "
    "session with status Ok, geometry intact and the operand-naming warning (MS31, AC-22's session-level "
    "half)") {
    // obj_import_test.cpp's own OI79 already proves this at the pure importModel() level. This is the
    // SAME property exercised through the actual disk-reading session path (TempDir + AssetDatabase +
    // ModelImportSession::service()) -- MS24's own precedent, restated for AC-22 instead of AC-52.
    // Deliberately NO usemtl (OI79's own reasoning): that would also trigger the library's OWN "material
    // not found" warning, which is real and correct but not what this case exists to isolate.
    const TempDir dir;
    writeFile(dir.join("chair.obj"), "mtllib chair.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    // Deliberately NO chair.mtl on disk.
    AssetDatabase db;
    GuidGenerator gen(208);
    db.rescan(dir.utf8(), dir.utf8(), gen);

    ModelImportSession session;
    session.setTarget("chair.obj", db.generation());
    session.service(dir.utf8(), db);
    CHECK(session.state() == SessionState::Imported);
    CHECK(session.result().status == ImportStatus::Ok);
    CHECK(session.result().model.materials.empty());
    REQUIRE(session.result().model.meshes.size() == 1);
    REQUIRE(session.result().model.meshes[0].primitives.size() == 1);
    REQUIRE(session.result().model.meshes[0].primitives[0].indices.size() == 3);  // geometry FULLY imported
    REQUIRE(session.result().warnings.size() == 1);
    CHECK(session.result().warnings[0].find("chair.mtl") != std::string::npos);
}

TEST_CASE("model_import_session: a .blend target is NotImportable and imports nothing (MS32, AC-63 corrected)") {
    const AssetDatabase db;  // never scanned -- a garbage root below proves nothing was read either
    ModelImportSession session;
    session.setTarget("statue.blend", 0);
    session.service("/this/path/must/never/be/opened", db);
    CHECK(session.state() == SessionState::NotImportable);
    CHECK(session.importCount() == 0);
}

TEST_CASE(
    "model_import_session: applySettings on an .obj writes the sidecar ONCE, atomically, with "
    "\"name\": \"obj\" -- a byte-identical re-apply writes nothing (MS33, AC-65)") {
    const TempDir dir;
    writeFile(dir.join("chair.obj"), "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    AssetDatabase db;
    GuidGenerator gen(209);
    db.rescan(dir.utf8(), dir.utf8(), gen);
    const Guid guid = *db.guidForPath("chair.obj");

    ModelImportSession session;
    session.setTarget("chair.obj", db.generation());
    session.service(dir.utf8(), db);
    REQUIRE(session.state() == SessionState::Imported);

    ImportSettings edited;
    edited.scale = 3.5F;
    session.setPendingSettings(edited);
    REQUIRE(session.canApply());
    const std::string error = session.applySettings(dir.utf8());
    CHECK(error.empty());

    const auto metaText = scene_golden::readBytes(dir.join("chair.obj.meta"));
    REQUIRE(metaText.ok);
    CHECK(metaText.text == writeMetaText(guid, edited, "obj", 1));
    CHECK(metaText.text.find(R"("name": "obj")") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(pathOf(dir.join("chair.obj.meta") + std::string(ATOMIC_TEMP_SUFFIX))));

    // A byte-identical re-apply (settings unchanged) writes NOTHING -- INV-M9: no new write path is
    // added for OBJ beyond the one applySettings() already owned.
    std::error_code ec;
    const std::filesystem::file_time_type beforeMtime =
        std::filesystem::last_write_time(pathOf(dir.join("chair.obj.meta")), ec);
    REQUIRE_FALSE(ec);
    session.setPendingSettings(edited);  // the SAME settings again
    const std::string secondError = session.applySettings(dir.utf8());
    CHECK(secondError.empty());
    const std::filesystem::file_time_type afterMtime =
        std::filesystem::last_write_time(pathOf(dir.join("chair.obj.meta")), ec);
    REQUIRE_FALSE(ec);
    CHECK(afterMtime == beforeMtime);
}
