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
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
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

// ---- task 3.2.4: the .blend arm's own scaffolding ------------------------------------------------
//
// NO CI LANE HAS BLENDER (R2), so the cases below use the real `cmake` executable as the tool the
// service resolves and spawns -- it exists on every runner by definition, its `--version` exits 0
// (which D14 then attempts rather than refuses, because the banner does not parse as a Blender
// version), and handed Blender's own argv it exits NON-zero without writing a status file, which is
// exactly the SourceRejected row. Nothing here sleeps and nothing reads a real environment variable.
constexpr std::string_view CMAKE_COMMAND = AERO_TEST_CMAKE_COMMAND;

// An env that resolves to EXACTLY ONE candidate -- the override, alone (AC-3).
[[nodiscard]] engine::editor::BlenderEnv overrideEnv(std::string_view binary) {
    engine::editor::BlenderEnv env;
    env.overridePath = std::string(binary);
    return env;
}

// <projectRoot>/Library/BlenderExports, assembled exactly as the arm assembles it.
[[nodiscard]] std::string exportDirOf(std::string_view projectRootUtf8) {
    return std::string(projectRootUtf8) + '/' + std::string(engine::editor::ASSET_CACHE_DIR_NAME) + '/' +
           std::string(engine::editor::BLENDER_EXPORT_DIR_NAME);
}

// The settings fingerprint, RE-DERIVED here from the two public primitives rather than by calling the
// production helper -- so a change to what goes into the fingerprint reddens these cases instead of
// silently moving with them.
[[nodiscard]] std::string fingerprintOfSettings(const engine::editor::ImportSettings& settings) {
    const std::string text = engine::editor::writeMetaText(engine::Guid{}, settings);
    return engine::formatContentHash(engine::hashBytes(std::as_bytes(std::span<const char>(text))));
}

// Drives service() until the Blender service leaves a transient state. BOUNDED by an iteration count,
// never by a clock, and it YIELDS rather than spins -- a pure spin competes with the very child it is
// waiting for (blender_service_test.cpp measured that directly).
constexpr int MAX_SERVICE_ITERATIONS = 5000000;

void serviceUntilSettled(engine::editor::ModelImportSession& session, std::string_view assetsRoot,
                         const engine::editor::AssetDatabase& db, float dt = 0.0F) {
    for (int i = 0; i < MAX_SERVICE_ITERATIONS; ++i) {
        session.service(assetsRoot, db, dt);
        const engine::editor::BlenderState state = session.blender().state();
        if (state != engine::editor::BlenderState::Probing && state != engine::editor::BlenderState::Converting) {
            return;
        }
        std::this_thread::yield();
    }
    FAIL("the Blender service never settled within the iteration budget");
}

}  // namespace

using engine::Guid;
using engine::GuidGenerator;
using engine::editor::AssetDatabase;
using engine::editor::AssetScanReport;
using engine::editor::ATOMIC_TEMP_SUFFIX;
using engine::editor::BlenderState;  // task 3.2.4
using engine::editor::currentHostOs;
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

    // task 3.2.4: SCOPED to service()'s OWN BODY, and that narrowing is a sharpening rather than a
    // relaxation. This case is about the TWO-PASS SHAPE, which lives in service(); the .blend arm added
    // by 3.2.4 lives in serviceBlend() and performs its own, deliberately UNGATED single read of the
    // GLB artifact (AC-44 -- the gate would answer TRUE for a ".glb" and must never be consulted
    // there). Counting the whole file would conflate the two and make "1" mean nothing. MS41 pins the
    // arm's own shape separately, so neither read is unproven.
    //
    // The trailing '(' is load-bearing: without it this also matches serviceBlend's definition line.
    std::size_t serviceOpenLine = code.size();
    for (std::size_t i = 0; i < code.size(); ++i) {
        if (code[i].find("void ModelImportSession::service(") != std::string::npos) {
            REQUIRE(serviceOpenLine == code.size());  // exactly ONE definition
            serviceOpenLine = i;
        }
    }
    REQUIRE(serviceOpenLine != code.size());
    const auto blockEndFrom = [&code](std::size_t startLine) {
        int braces = 0;
        for (std::size_t i = startLine; i < code.size(); ++i) {
            for (const char c : code[i]) {
                if (c == '{') {
                    ++braces;
                } else if (c == '}') {
                    --braces;
                    if (braces == 0) {
                        return i;
                    }
                }
            }
        }
        return code.size();
    };
    const std::size_t serviceCloseLine = blockEndFrom(serviceOpenLine);
    REQUIRE(serviceCloseLine != code.size());

    std::size_t gateOpenLine = code.size();
    for (std::size_t i = serviceOpenLine; i <= serviceCloseLine; ++i) {
        if (code[i].find("modelImporterNeedsExternalBuffers(leaf)") != std::string::npos) {
            REQUIRE(gateOpenLine == code.size());  // exactly ONE gate line in service()'s body
            gateOpenLine = i;
        }
    }
    REQUIRE(gateOpenLine != code.size());  // the gate exists at all
    // and NOWHERE ELSE in the file either -- in particular not inside the .blend arm, where consulting
    // it would be the AC-44 defect (seed S26).
    std::size_t gateSitesInFile = 0;
    for (const std::string& line : code) {
        if (line.find("modelImporterNeedsExternalBuffers") != std::string::npos) {
            ++gateSitesInFile;
        }
    }
    CHECK(gateSitesInFile == 1);

    // Walk forward from the gate line, tracking brace depth, to find where its block CLOSES.
    const std::size_t gateCloseLine = blockEndFrom(gateOpenLine);
    REQUIRE(gateCloseLine != code.size());  // the gate's own block is well-formed and closes
    REQUIRE(gateCloseLine <= serviceCloseLine);

    std::size_t readsOutsideGate = 0;
    std::size_t readsInsideGate = 0;
    for (std::size_t i = serviceOpenLine; i <= serviceCloseLine; ++i) {
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
    for (std::size_t i = gateCloseLine; i <= serviceCloseLine; ++i) {
        if (code[i].find("ImportDepth::Full, externals") != std::string::npos) {
            pass2Line = i;
            break;
        }
    }
    CHECK(pass2Line > gateCloseLine);
    CHECK(pass2Line <= serviceCloseLine);
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

TEST_CASE(
    "model_import_session: a .blend with no AssetDatabase record reaches NeedsConversion with no "
    "identity, and reads nothing (MS32, task 3.2.4 AC-27)") {
    // REWRITTEN at task 3.2.4, in the SAME commit as the .blend arm. Before that this case asserted
    // NotImportable -- which the arm makes false -- and, worse, under the spec's own (rejected) design
    // it would have gone on PASSING for a COMPLETELY DIFFERENT REASON: not because .blend is unclaimed,
    // but because an unscanned database yields a nil GUID. A case that passes for a new reason is worse
    // than one that fails, so it is retitled and re-aimed rather than left green.
    const AssetDatabase db;  // never scanned -- a garbage root below proves nothing was read either
    ModelImportSession session;
    session.setTarget("statue.blend", 0);
    session.service("/this/path/must/never/be/opened", db);
    CHECK(session.state() == SessionState::NeedsConversion);
    CHECK_FALSE(session.targetHasIdentity());
    CHECK(session.importCount() == 0);
    // AC-27: ZERO processes of any kind. The split matters -- "zero processes" as one number is
    // unsatisfiable while the version probe is itself a process (§A-9).
    CHECK(session.blender().exportRunCount() == 0);
    CHECK(session.blender().probeRunCount() == 0);
    // and the service was never even resolved: no environment read, no candidate path stat'ed.
    CHECK(session.blender().state() == BlenderState::Unknown);
    CHECK(session.blender().searchedPaths().empty());
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

// ---- task 3.2.4: the .blend arm (MS34-MS40) -------------------------------------------------------

TEST_CASE(
    "model_import_session: a REAL scanned .blend with a valid GUID reaches NeedsConversion WITH an "
    "identity, and still spawns nothing (MS34, task 3.2.4 AC-27's mirror)") {
    // MS32's mirror, and without the pair "the .blend arm exists at all" has no discriminating proof:
    // MS32 alone would pass for a session that simply refused every .blend.
    const TempDir dir;
    const std::string assets = dir.join("assets");
    std::error_code ec;
    std::filesystem::create_directories(pathOf(assets), ec);
    REQUIRE_FALSE(ec);
    writeFile(assets + "/statue.blend", "not parsed by anything, ever");

    AssetDatabase db;
    GuidGenerator gen(240);
    db.rescan(dir.utf8(), assets, gen);
    REQUIRE(db.guidForPath("statue.blend").has_value());

    ModelImportSession session;
    session.setTarget("statue.blend", db.generation());
    session.service(assets, db);
    CHECK(session.state() == SessionState::NeedsConversion);
    CHECK(session.targetHasIdentity());  // <-- the half MS32 cannot have
    CHECK(session.importCount() == 0);
    CHECK(session.blender().exportRunCount() == 0);
    CHECK(session.blender().probeRunCount() == 0);
    // NOT NotImportable, and never again: that enumerator means "no importer claims this file type",
    // which is false for a .blend after this task.
    CHECK(session.state() != SessionState::NotImportable);
}

TEST_CASE(
    "model_import_session: a conversion request against a MISSING Blender is dropped without spawning "
    "(MS35, task 3.2.4 AC-30)") {
    const TempDir dir;
    const std::string assets = dir.join("assets");
    std::error_code ec;
    std::filesystem::create_directories(pathOf(assets), ec);
    REQUIRE_FALSE(ec);
    writeFile(assets + "/statue.blend", "opaque bytes");
    AssetDatabase db;
    GuidGenerator gen(241);
    db.rescan(dir.utf8(), assets, gen);

    ModelImportSession session;
    session.setTarget("statue.blend", db.generation());
    session.service(assets, db);
    REQUIRE(session.state() == SessionState::NeedsConversion);

    // An override naming a path that does not exist yields EXACTLY ONE candidate, which does not
    // resolve -> ToolMissing, with the searched list retained for the panel.
    session.blenderMutable().resolve(currentHostOs(), overrideEnv(dir.join("no-such-blender")),
                                     exportDirOf(dir.utf8()));
    REQUIRE(session.blender().state() == BlenderState::ToolMissing);
    CHECK_FALSE(session.blender().searchedPaths().empty());

    session.requestConversion();
    session.service(assets, db);
    CHECK(session.state() == SessionState::NeedsConversion);  // still offering, never Converting
    CHECK(session.blender().exportRunCount() == 0);
    CHECK(session.blender().probeRunCount() == 0);
}

TEST_CASE(
    "model_import_session: the serviced-guard exception is NARROW -- an Imported .blend costs nothing "
    "over ten ticks, while a Converting one is polled on every one (MS36, task 3.2.4 seed S37)") {
    const TempDir dir;
    const std::string assets = dir.join("assets");
    std::error_code ec;
    std::filesystem::create_directories(pathOf(assets), ec);
    REQUIRE_FALSE(ec);
    writeFile(assets + "/statue.blend", "opaque bytes");
    AssetDatabase db;
    GuidGenerator gen(242);
    db.rescan(dir.utf8(), assets, gen);

    ModelImportSession session;
    session.setTarget("statue.blend", db.generation());

    SUBCASE("half A -- a settled .blend takes the early return, exactly as a .gltf does") {
        session.service(assets, db);
        REQUIRE(session.state() == SessionState::NeedsConversion);
        const std::size_t importsAfterFirst = session.importCount();
        for (int i = 0; i < 10; ++i) {
            session.service(assets, db, 0.016F);
        }
        CHECK(session.importCount() == importsAfterFirst);
        // and nothing was resolved, probed or spawned by those ten ticks either.
        CHECK(session.blender().state() == BlenderState::Unknown);
        CHECK(session.blender().probeRunCount() == 0);
        CHECK(session.blender().exportRunCount() == 0);
    }

    SUBCASE("half B -- a Converting .blend is re-entered, and the SPAWN itself proves poll() ran") {
        session.service(assets, db);
        REQUIRE(session.state() == SessionState::NeedsConversion);
        session.blenderMutable().resolve(currentHostOs(), overrideEnv(CMAKE_COMMAND), exportDirOf(dir.utf8()));
        REQUIRE(session.blender().state() == BlenderState::Probing);
        // The probe is a CHILD PROCESS, and it is driven entirely by service() -- which is only
        // reachable at all because the guard's exception let the arm back in after `serviced` was set.
        serviceUntilSettled(session, assets, db);
        REQUIRE(session.blender().state() == BlenderState::Ready);
        CHECK(session.blender().probeRunCount() == 1);

        session.requestConversion();
        session.service(assets, db);
        REQUIRE(session.state() == SessionState::Converting);
        // The request is RECORDED, not spawned: poll() is the only spawn site (INV-B15).
        CHECK(session.blender().exportRunCount() == 0);

        // The NEXT tick is the one that matters: `serviced` is already true and the target has not
        // changed, so without the exception this call would early-return and the export would never
        // start. The counter moving is the proof that poll() ran.
        session.service(assets, db, 0.016F);
        CHECK(session.blender().exportRunCount() == 1);
    }
}

TEST_CASE(
    "model_import_session: requestConversion() and cancelConversion() are ONE-SHOTS, consumed exactly "
    "once (MS37, task 3.2.4 AC-39's session half)") {
    const TempDir dir;
    const std::string assets = dir.join("assets");
    std::error_code ec;
    std::filesystem::create_directories(pathOf(assets), ec);
    REQUIRE_FALSE(ec);
    writeFile(assets + "/statue.blend", "opaque bytes");
    AssetDatabase db;
    GuidGenerator gen(243);
    db.rescan(dir.utf8(), assets, gen);

    ModelImportSession session;
    session.setTarget("statue.blend", db.generation());
    session.service(assets, db);
    REQUIRE(session.state() == SessionState::NeedsConversion);

    // A request made while there is no usable Blender is CONSUMED and DROPPED, never queued.
    session.blenderMutable().resolve(currentHostOs(), overrideEnv(dir.join("no-such-blender")),
                                     exportDirOf(dir.utf8()));
    REQUIRE(session.blender().state() == BlenderState::ToolMissing);
    session.requestConversion();
    session.service(assets, db);
    REQUIRE(session.blender().exportRunCount() == 0);

    // Now make the tool usable WITHOUT making a new request. A queued request would spawn here; a
    // consumed one cannot.
    session.blenderMutable().resolve(currentHostOs(), overrideEnv(CMAKE_COMMAND), exportDirOf(dir.utf8()));
    serviceUntilSettled(session, assets, db);
    REQUIRE(session.blender().state() == BlenderState::Ready);
    for (int i = 0; i < 5; ++i) {
        session.service(assets, db, 0.016F);
    }
    CHECK(session.blender().exportRunCount() == 0);
    CHECK(session.state() == SessionState::NeedsConversion);

    // cancelConversion() outside a run is likewise consumed and harmless.
    session.cancelConversion();
    session.service(assets, db);
    CHECK(session.state() == SessionState::NeedsConversion);
    CHECK(session.blender().exportRunCount() == 0);
}

TEST_CASE(
    "model_import_session: a .blend whose REAL record carries a nil GUID is NeedsConversion with no "
    "identity, and reads nothing (MS38, task 3.2.4 AC-27, seed S29)") {
    // The non-vacuous half of AC-27: a record that genuinely EXISTS and whose sidecar is invalid, so
    // the nil GUID is a property of the asset rather than of an unscanned database. D7 forbids
    // repairing it, so this state is permanent until the user fixes the sidecar.
    const TempDir dir;
    const std::string assets = dir.join("assets");
    std::error_code ec;
    std::filesystem::create_directories(pathOf(assets), ec);
    REQUIRE_FALSE(ec);
    writeFile(assets + "/statue.blend", "opaque bytes");
    writeFile(assets + "/statue.blend.meta", "{ this is not valid json at all");

    AssetDatabase db;
    GuidGenerator gen(244);
    db.rescan(dir.utf8(), assets, gen);
    const engine::editor::AssetRecord* const record = db.findByPath("statue.blend");
    REQUIRE(record != nullptr);
    REQUIRE_FALSE(record->guid.valid());  // an invalid sidecar is never repaired (3.1.1 D7)

    ModelImportSession session;
    session.setTarget("statue.blend", db.generation());
    session.service(assets, db);
    CHECK(session.state() == SessionState::NeedsConversion);
    CHECK_FALSE(session.targetHasIdentity());
    CHECK(session.importCount() == 0);
    CHECK(session.blender().exportRunCount() == 0);
    CHECK(session.blender().probeRunCount() == 0);

    // Even an explicit request cannot start anything: there is nowhere to cache a conversion.
    session.requestConversion();
    session.service(assets, db);
    CHECK(session.state() == SessionState::NeedsConversion);
    CHECK(session.blender().exportRunCount() == 0);
}

TEST_CASE(
    "model_import_session: applySettings on a .blend writes the sidecar with the EMPTY identity, and "
    "that is correct (MS39, task 3.2.4 D16)") {
    // MEASURED, and it CORRECTS this task's own plan, which predicted "name": "gltf" here. The
    // prediction assumed applySettings would fall through to writeMetaText's DEFAULTED importer
    // parameters; it does not. Task 3.2.2 made applySettings pass modelImporterIdentity(leaf)
    // EXPLICITLY -- exactly so an .fbx stops recording a borrowed "gltf" -- and that function returns
    // ("", 0) for a .blend, because the identity table has no .blend arm.
    //
    // The empty pair is the RIGHT answer, not a shortfall: it says "no importer claims this file",
    // which is precisely true. Nothing ever probes a .blend (D15), so its import-cache entry stays at
    // ""/0 and cannot oscillate, and the sidecar now agrees with the cache instead of contradicting it.
    // The block is PREFERENCE; its name field is not what selects an importer.
    const TempDir dir;
    const std::string assets = dir.join("assets");
    std::error_code ec;
    std::filesystem::create_directories(pathOf(assets), ec);
    REQUIRE_FALSE(ec);
    writeFile(assets + "/statue.blend", "opaque bytes");
    AssetDatabase db;
    GuidGenerator gen(245);
    db.rescan(dir.utf8(), assets, gen);
    const Guid guid = *db.guidForPath("statue.blend");

    ModelImportSession session;
    session.setTarget("statue.blend", db.generation());
    session.service(assets, db);
    REQUIRE(session.state() == SessionState::NeedsConversion);

    ImportSettings edited;
    edited.scale = 2.5F;
    session.setPendingSettings(edited);
    REQUIRE(session.canApply());
    CHECK(session.applySettings(assets).empty());

    const auto metaText = scene_golden::readBytes(assets + "/statue.blend.meta");
    REQUIRE(metaText.ok);
    CHECK(metaText.text == writeMetaText(guid, edited, "", 0));
    CHECK(metaText.text.find(R"("name": "")") != std::string::npos);
    CHECK(metaText.text.find(R"("name": "gltf")") == std::string::npos);
    // The settings themselves round-trip exactly as they do for every other format.
    CHECK(metaText.text.find(R"("scale": 2.5)") != std::string::npos);
}

TEST_CASE(
    "model_import_session: service()'s new deltaSeconds parameter is TRAILING and DEFAULTED -- the two "
    "call shapes are indistinguishable for a non-.blend target (MS40, task 3.2.4 AC-45)") {
    // AC-45's mechanical half is that all 42 pre-existing `.service(` call sites in this file compile
    // UNEDITED. This case asserts the behavioural half: supplying the argument explicitly changes
    // nothing at all for a target that has no Blender path.
    const TempDir dir;
    writeFile(dir.join("a.gltf"), R"({"asset":{"version":"2.0"}})");
    AssetDatabase db;
    GuidGenerator gen(246);
    db.rescan(dir.utf8(), dir.utf8(), gen);

    ModelImportSession defaulted;
    defaulted.setTarget("a.gltf", db.generation());
    defaulted.service(dir.utf8(), db);

    ModelImportSession explicitDt;
    explicitDt.setTarget("a.gltf", db.generation());
    explicitDt.service(dir.utf8(), db, 0.016F);

    CHECK(defaulted.state() == explicitDt.state());
    CHECK(defaulted.state() == SessionState::Imported);
    CHECK(defaulted.importCount() == explicitDt.importCount());
    CHECK(defaulted.result().status == explicitDt.result().status);
    // and neither touched the Blender service in any way.
    CHECK(defaulted.blender().state() == BlenderState::Unknown);
    CHECK(explicitDt.blender().state() == BlenderState::Unknown);
}

TEST_CASE(
    "model_import_session: the .blend arm's own shape -- ONE importModel call, ONE readFileBytes, and "
    "modelImporterNeedsExternalBuffers NEVER consulted (MS41, task 3.2.4 AC-44/INV-B16, seed S26)") {
    // A SOURCE-TEXT proof, for the same reason MS22 is one: routing the artifact through the two-pass
    // driver instead of the direct Full call produces an IDENTICAL ImportResult for any ordinary GLB,
    // because an ordinary GLB names no external URI at all. BS41's fixture is what makes the defect
    // BEHAVIOURALLY visible; this case makes the STRUCTURE itself assertable, so seed S26 reddens on
    // both axes rather than resting entirely on one fixture being authored exactly right.
    constexpr std::string_view SOURCE_PATH = AERO_EDITOR_SRC_DIR "/model_import_session.cpp";
    const engine::editor::FileReadResult read = readTextFile(SOURCE_PATH);
    REQUIRE(read.text.has_value());
    const std::string& text = *read.text;

    std::vector<std::string> code;
    std::string_view remaining = text;
    while (true) {
        const std::size_t newline = remaining.find('\n');
        const std::string_view line = newline == std::string_view::npos ? remaining : remaining.substr(0, newline);
        const std::size_t commentStart = line.find("//");
        code.emplace_back(commentStart == std::string_view::npos ? line : line.substr(0, commentStart));
        if (newline == std::string_view::npos) {
            break;
        }
        remaining.remove_prefix(newline + 1U);
    }

    std::size_t armOpen = code.size();
    for (std::size_t i = 0; i < code.size(); ++i) {
        if (code[i].find("void ModelImportSession::serviceBlend(") != std::string::npos) {
            REQUIRE(armOpen == code.size());  // exactly ONE definition
            armOpen = i;
        }
    }
    REQUIRE(armOpen != code.size());

    int braces = 0;
    std::size_t armClose = code.size();
    for (std::size_t i = armOpen; i < code.size() && armClose == code.size(); ++i) {
        for (const char c : code[i]) {
            if (c == '{') {
                ++braces;
            } else if (c == '}') {
                --braces;
                if (braces == 0) {
                    armClose = i;
                    break;
                }
            }
        }
    }
    REQUIRE(armClose != code.size());

    std::size_t importCalls = 0;
    std::size_t reads = 0;
    std::size_t gateConsults = 0;
    std::size_t fullDepth = 0;
    std::size_t emptyDir = 0;
    for (std::size_t i = armOpen; i <= armClose; ++i) {
        if (code[i].find("importModel(") != std::string::npos) {
            ++importCalls;
        }
        if (code[i].find("readFileBytes(") != std::string::npos) {
            ++reads;
        }
        if (code[i].find("modelImporterNeedsExternalBuffers") != std::string::npos) {
            ++gateConsults;
        }
        if (code[i].find("ImportDepth::Full") != std::string::npos) {
            ++fullDepth;
        }
        if (code[i].find(R"(/*assetRelativeDir=*/"")") != std::string::npos) {
            ++emptyDir;
        }
    }
    CHECK(importCalls == 1);   // ONE call -- never a Structure pass followed by a Full one
    CHECK(reads == 1);         // the artifact's own bytes, and nothing else
    CHECK(gateConsults == 0);  // AC-44: DELIBERATELY not consulted, and never by accident
    CHECK(fullDepth == 1);     // at Full depth
    CHECK(emptyDir == 1);      // with an EMPTY assetRelativeDir -- Library/ has no assets-relative dir
}
