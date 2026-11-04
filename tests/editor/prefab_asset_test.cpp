// tests/editor/prefab_asset_test.cpp -- task 4.4.4: InstantiatePrefabCommand and the prefab text bridge
// (PE1-PE8) and the save below the editor (PE9-PE17). A TU of aero_editor_shell_test, which supplies main()
// from shell_test.cpp -- do NOT define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// UNGATED and tier-0: it compiles in every configuration, with no window, no GPU and no ImGui. The command cases
// drive a REAL CommandStack against a real CommandContext{world, selection, roots}, over a Prefab made through
// Prefab::capture, so they need no parse. The two text cases ask sceneIoAvailable() first and assert the
// tools-OFF answer when it is false, which is how this file holds no preprocessor conditional.
//
// <ostream> is included because std::string_view values are compared. No toString overload is added anywhere.
#include <aero/core/guid.hpp>
#include <aero/core/log.hpp>
#include <aero/core/math.hpp>
#include <aero/editor/asset_actions.hpp>
#include <aero/editor/asset_commands.hpp>
#include <aero/editor/asset_database.hpp>
#include <aero/editor/asset_view.hpp>
#include <aero/editor/command_stack.hpp>
#include <aero/editor/entity_commands.hpp>
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/prefab_asset.hpp>
#include <aero/editor/project_files.hpp>
#include <aero/editor/scene_session.hpp>
#include <aero/editor/selection.hpp>
#include <aero/reflect/scene_format.hpp>
#include <aero/scene/scene.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using engine::Entity;
using engine::Prefab;
using engine::Quat;
using engine::Transform;
using engine::Vec3;
using engine::World;
using engine::editor::AssetDatabase;
using engine::editor::CommandContext;
using engine::editor::CommandStack;
using engine::editor::INSTANTIATE_PREFAB_COMMAND_LABEL;
using engine::editor::InstantiatePrefabCommand;
using engine::editor::prefabFileStem;
using engine::editor::prefabFromText;
using engine::editor::PrefabReadResult;
using engine::editor::PrefabSaveResult;
using engine::editor::prefabToText;
using engine::editor::RenameEntityCommand;
using engine::editor::RootOrder;
using engine::editor::savePrefabFile;
using engine::editor::sceneIoAvailable;
using engine::editor::Selection;
using engine::editor::uniqueAssetFileName;

namespace {

[[nodiscard]] Entity makeNamed(World& world, std::string_view name, Entity parent, Vec3 position) {
    const Entity entity = world.create();
    REQUIRE(world.setName(entity, name));
    Transform transform;
    transform.position = position;
    REQUIRE(world.add<Transform>(entity, transform) != nullptr);
    if (parent.valid()) {
        REQUIRE(world.setParent(entity, parent));
    }
    return entity;
}

// Crate(Lid(Hinge), Latch): four entities. The root is rotated and scaled so the placement composition is visible, and
// it sits away from the origin so that capture's zeroing of a single root's position is visible too.
[[nodiscard]] Prefab makeCratePrefab() {
    World source;
    const Entity crate = makeNamed(source, "Crate", Entity{}, Vec3{5.0F, 0.0F, 0.0F});
    auto* crateTransform = source.get<Transform>(crate);
    REQUIRE(crateTransform != nullptr);
    crateTransform->rotation = engine::fromAxisAngle(Vec3{0.0F, 1.0F, 0.0F}, engine::radians(30.0F));
    crateTransform->scale = Vec3{2.0F, 2.0F, 2.0F};
    const Entity lid = makeNamed(source, "Lid", crate, Vec3{0.0F, 1.0F, 0.0F});
    static_cast<void>(makeNamed(source, "Hinge", lid, Vec3{0.5F, 0.0F, 0.0F}));
    static_cast<void>(makeNamed(source, "Latch", crate, Vec3{1.0F, 0.0F, 0.0F}));
    const std::array<Entity, 1> roots{crate};
    std::optional<Prefab> prefab = Prefab::capture(source, roots, "unused");
    REQUIRE(prefab.has_value());
    return std::move(*prefab);
}

[[nodiscard]] Transform dropPlacement() {
    Transform placement;
    placement.position = Vec3{10.0F, 0.0F, -5.0F};
    placement.scale = Vec3{3.0F, 1.0F, 1.0F};
    return placement;
}

[[nodiscard]] std::size_t liveEntityCount(const World& world) {
    std::size_t count = 0;
    world.eachEntity([&count](Entity) { ++count; });
    return count;
}

[[nodiscard]] Entity childNamed(const World& world, Entity parent, std::string_view name) {
    Entity found;
    world.eachChild(parent, [&](Entity child) {
        if (world.name(child) == name) {
            found = child;
        }
    });
    return found;
}

[[nodiscard]] std::vector<Entity> childrenOf(const World& world, Entity parent) {
    std::vector<Entity> out;
    world.eachChild(parent, [&out](Entity child) { out.push_back(child); });
    return out;
}

[[nodiscard]] std::vector<std::string> childNames(const World& world, Entity parent) {
    std::vector<std::string> out;
    world.eachChild(parent, [&](Entity child) { out.emplace_back(world.name(child)); });
    return out;
}

// Counts every record a case provokes, through the public setLogCallback seam.
class LogCount {
public:
    LogCount() {
        engine::setLogCallback([this](const engine::LogRecord&) { ++records; });
    }
    LogCount(const LogCount&) = delete;
    LogCount& operator=(const LogCount&) = delete;
    LogCount(LogCount&&) = delete;
    LogCount& operator=(LogCount&&) = delete;
    ~LogCount() { engine::setLogCallback(nullptr); }

    [[nodiscard]] int count() const noexcept { return records; }

private:
    int records = 0;
};

}  // namespace

TEST_CASE("prefab_asset: the first redo copies the tree under placement o stored and selects the root (PE1)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    CommandStack stack;

    auto command = std::make_unique<InstantiatePrefabCommand>(makeCratePrefab(), Entity{}, dropPlacement(),
                                                              std::span<const Entity>{});
    const InstantiatePrefabCommand* raw = command.get();
    CHECK_FALSE(raw->createdRoot().valid());
    REQUIRE(stack.push(ctx, std::move(command)));

    CHECK(liveEntityCount(world) == 4);
    const Entity root = raw->createdRoot();
    REQUIRE(root.valid());
    CHECK(world.name(root) == "Crate");
    CHECK_FALSE(world.parent(root).valid());

    // capture stored the single root at position zero, so the composed position is the placement's own, and the scale
    // is the componentwise product. The stored rotation survives an identity placement exactly.
    const Transform* rootTransform = world.get<Transform>(root);
    REQUIRE(rootTransform != nullptr);
    CHECK(rootTransform->position == Vec3{10.0F, 0.0F, -5.0F});
    CHECK(rootTransform->scale == Vec3{6.0F, 2.0F, 2.0F});
    CHECK(rootTransform->rotation == engine::fromAxisAngle(Vec3{0.0F, 1.0F, 0.0F}, engine::radians(30.0F)));

    // The children are untouched by the placement.
    const Entity lid = childNamed(world, root, "Lid");
    REQUIRE(lid.valid());
    const Transform* lidTransform = world.get<Transform>(lid);
    REQUIRE(lidTransform != nullptr);
    CHECK(lidTransform->position == Vec3{0.0F, 1.0F, 0.0F});
    CHECK(lidTransform->scale == Vec3{1.0F, 1.0F, 1.0F});
    CHECK(world.name(childNamed(world, lid, "Hinge")) == "Hinge");

    // The ONE selected entity is the root, never a child.
    CHECK(selection.count() == 1);
    CHECK(selection.primary() == root);
    CHECK_FALSE(selection.contains(lid));

    CHECK(INSTANTIATE_PREFAB_COMMAND_LABEL == std::string_view("Instantiate Prefab"));
    CHECK(stack.undoLabel() == INSTANTIATE_PREFAB_COMMAND_LABEL);
    CHECK(raw->label() == std::string_view("Instantiate Prefab"));
    CHECK(raw->label() != engine::editor::INSTANTIATE_ASSET_COMMAND_LABEL);
}

TEST_CASE("prefab_asset: undo removes the whole tree, and redo brings back the SAME handles (PE2)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    CommandStack stack;

    const Entity previouslySelected = world.create();
    selection.set(previouslySelected);
    const std::vector<Entity> before(selection.entities().begin(), selection.entities().end());
    const std::size_t baseline = liveEntityCount(world);

    auto command = std::make_unique<InstantiatePrefabCommand>(makeCratePrefab(), Entity{}, dropPlacement(), before);
    const InstantiatePrefabCommand* raw = command.get();
    REQUIRE(stack.push(ctx, std::move(command)));

    const Entity root = raw->createdRoot();
    const Entity lid = childNamed(world, root, "Lid");
    const Entity hinge = childNamed(world, lid, "Hinge");
    const Entity latch = childNamed(world, root, "Latch");
    REQUIRE(hinge.valid());
    REQUIRE(latch.valid());
    CHECK(selection.primary() == root);

    REQUIRE(stack.undo(ctx));
    CHECK(liveEntityCount(world) == baseline);
    CHECK_FALSE(world.alive(root));
    CHECK_FALSE(world.alive(lid));
    CHECK_FALSE(world.alive(hinge));
    CHECK_FALSE(world.alive(latch));
    CHECK(selection.count() == 1);
    CHECK(selection.primary() == previouslySelected);

    REQUIRE(stack.redo(ctx));
    CHECK(liveEntityCount(world) == baseline + 4);
    CHECK(raw->createdRoot() == root);  // a re-instantiation would mint fresh handles
    CHECK(world.alive(root));
    CHECK(world.alive(lid));
    CHECK(world.alive(hinge));
    CHECK(world.alive(latch));
    CHECK(world.parent(lid) == root);
    CHECK(world.parent(hinge) == lid);
    CHECK(selection.count() == 1);
    CHECK(selection.primary() == root);
}

TEST_CASE("prefab_asset: a rename pushed after the drop survives undo, undo, redo, redo (S10, PE3)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    CommandStack stack;

    auto command = std::make_unique<InstantiatePrefabCommand>(makeCratePrefab(), Entity{}, dropPlacement(),
                                                              std::span<const Entity>{});
    const InstantiatePrefabCommand* raw = command.get();
    REQUIRE(stack.push(ctx, std::move(command)));
    const Entity lid = childNamed(world, raw->createdRoot(), "Lid");
    REQUIRE(lid.valid());

    REQUIRE(stack.push(ctx, std::make_unique<RenameEntityCommand>(lid, "Lid", "Lid Renamed")));
    CHECK(world.name(lid) == "Lid Renamed");

    REQUIRE(stack.undo(ctx));  // the rename
    REQUIRE(stack.undo(ctx));  // the drop
    CHECK_FALSE(world.alive(lid));
    REQUIRE(stack.redo(ctx));  // the drop
    REQUIRE(stack.redo(ctx));  // the rename, against the handle it recorded
    CHECK(world.alive(lid));
    CHECK(world.name(lid) == "Lid Renamed");
}

TEST_CASE("prefab_asset: under a parent the copy is linked and keeps its place among the siblings (PE4)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    CommandStack stack;

    const Entity parent = makeNamed(world, "Shelf", Entity{}, Vec3{});
    static_cast<void>(makeNamed(world, "A", parent, Vec3{}));
    static_cast<void>(makeNamed(world, "B", parent, Vec3{}));
    const Entity loose = makeNamed(world, "C", Entity{}, Vec3{});

    auto command =
        std::make_unique<InstantiatePrefabCommand>(makeCratePrefab(), parent, Transform{}, std::span<const Entity>{});
    const InstantiatePrefabCommand* raw = command.get();
    REQUIRE(stack.push(ctx, std::move(command)));
    const Entity root = raw->createdRoot();
    CHECK(world.parent(root) == parent);
    CHECK(childNames(world, parent) == std::vector<std::string>{"A", "B", "Crate"});

    REQUIRE(stack.undo(ctx));
    CHECK(childNames(world, parent) == std::vector<std::string>{"A", "B"});
    // A sibling attached behind the history's back after the undo (an existing entity, because a new one would take
    // the freed index): a restore that appends would land after it.
    REQUIRE(world.setParent(loose, parent));
    REQUIRE(stack.redo(ctx));
    CHECK(raw->createdRoot() == root);
    CHECK(world.parent(root) == parent);
    CHECK(childNames(world, parent) == std::vector<std::string>{"A", "B", "Crate", "C"});
}

TEST_CASE("prefab_asset: a dead parent refuses, changing nothing and recording nothing (PE5)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    CommandStack stack;

    const Entity keep = world.create();
    selection.set(keep);
    const Entity doomed = world.create();
    world.destroy(doomed);
    const std::size_t before = liveEntityCount(world);
    const std::vector<Entity> selectionBefore(selection.entities().begin(), selection.entities().end());

    // Driven directly: a push that returns false destroys the command it was handed.
    InstantiatePrefabCommand command(makeCratePrefab(), doomed, dropPlacement(), selectionBefore);
    CHECK_FALSE(command.redo(ctx));
    CHECK_FALSE(command.createdRoot().valid());
    CHECK(liveEntityCount(world) == before);
    CHECK(selection.count() == 1);
    CHECK(selection.primary() == keep);

    CHECK_FALSE(stack.push(
        ctx, std::make_unique<InstantiatePrefabCommand>(makeCratePrefab(), doomed, dropPlacement(), selectionBefore)));
    CHECK(liveEntityCount(world) == before);
    CHECK(stack.count() == 0);
    CHECK(selection.count() == 1);
    CHECK(selection.primary() == keep);

    // Undo before any successful redo is a refusal that changes nothing either.
    CHECK_FALSE(command.undo(ctx));
    CHECK(liveEntityCount(world) == before);
}

TEST_CASE("prefab_asset: a root keeps its display slot across undo and redo with a reconcile between (PE6)") {
    World world;
    Selection selection;
    RootOrder roots;
    CommandContext ctx{world, selection, roots};
    CommandStack stack;

    const Entity a = world.create();
    const Entity b = world.create();
    roots.reconcile(world);
    REQUIRE(roots.indexOf(a) == 0);
    REQUIRE(roots.indexOf(b) == 1);

    auto command = std::make_unique<InstantiatePrefabCommand>(makeCratePrefab(), Entity{}, dropPlacement(),
                                                              std::span<const Entity>{});
    const InstantiatePrefabCommand* raw = command.get();
    REQUIRE(stack.push(ctx, std::move(command)));
    roots.reconcile(world);
    const Entity root = raw->createdRoot();
    REQUIRE(roots.indexOf(root) == 2);

    // A later root makes the case discriminating: with the copy already last, a lost slot would look like a restore.
    const Entity d = world.create();
    roots.reconcile(world);
    REQUIRE(roots.indexOf(d) == 3);

    REQUIRE(stack.undo(ctx));
    roots.reconcile(world);
    CHECK(roots.indexOf(a) == 0);
    CHECK(roots.indexOf(b) == 1);
    CHECK(roots.indexOf(d) == 2);

    REQUIRE(stack.redo(ctx));
    CHECK(roots.indexOf(raw->createdRoot()) == 2);  // its OWN row, not appended after d
    CHECK(roots.indexOf(d) == 3);
    roots.reconcile(world);
    CHECK(roots.indexOf(raw->createdRoot()) == 2);
    CHECK(roots.indexOf(d) == 3);
}

TEST_CASE("prefab_asset: the text bridge round-trips one root, and the text is a scene document (PE7)") {
    const Prefab prefab = makeCratePrefab();
    const std::optional<std::string> text = prefabToText(prefab);

    if (!sceneIoAvailable()) {
        // Tools OFF: no writer exists, and the reader says why in words.
        CHECK_FALSE(text.has_value());
        const PrefabReadResult refused = prefabFromText("{}");
        CHECK_FALSE(refused.prefab.has_value());
        CHECK_FALSE(refused.message.empty());
        return;
    }

    REQUIRE(text.has_value());
    // The text is a scene v1 document: the engine's own parser accepts it, with no prefab-specific key.
    const engine::SceneParseResult parsed = engine::parseScene(*text);
    CHECK(parsed.document.has_value());

    const PrefabReadResult read = prefabFromText(*text);
    REQUIRE(read.prefab.has_value());
    CHECK(read.message.empty());
    const Prefab& again = *read.prefab;
    CHECK(again.entityCount() == prefab.entityCount());
    const World& before = prefab.world();
    const World& after = again.world();
    CHECK(after.name(again.root()) == before.name(prefab.root()));
    CHECK(childNames(after, again.root()) == childNames(before, prefab.root()));

    const auto* storedRoot = before.get<Transform>(prefab.root());
    const auto* readRoot = after.get<Transform>(again.root());
    REQUIRE(storedRoot != nullptr);
    REQUIRE(readRoot != nullptr);
    CHECK(readRoot->position == storedRoot->position);
    CHECK(readRoot->rotation == storedRoot->rotation);
    CHECK(readRoot->scale == storedRoot->scale);

    const Entity lid = childNamed(after, again.root(), "Lid");
    REQUIRE(lid.valid());
    const auto* readLid = after.get<Transform>(lid);
    REQUIRE(readLid != nullptr);
    CHECK(readLid->position == Vec3{0.0F, 1.0F, 0.0F});
    CHECK(childNamed(after, lid, "Hinge").valid());
}

TEST_CASE("prefab_asset: a bad file is named by position or by the one-root rule, and nothing is logged (PE8)") {
    if (!sceneIoAvailable()) {
        const PrefabReadResult refused = prefabFromText("{");
        CHECK_FALSE(refused.prefab.has_value());
        CHECK_FALSE(refused.message.empty());
        return;
    }

    const World none;
    World two;
    static_cast<void>(makeNamed(two, "One", Entity{}, Vec3{}));
    static_cast<void>(makeNamed(two, "Two", Entity{}, Vec3{}));
    const std::string zeroRoots = engine::editor::sceneToText(none).value_or(std::string{});
    const std::string twoRoots = engine::editor::sceneToText(two).value_or(std::string{});
    REQUIRE_FALSE(zeroRoots.empty());
    REQUIRE_FALSE(twoRoots.empty());

    const LogCount log;

    const PrefabReadResult broken = prefabFromText("{ \"entities\": [");
    CHECK_FALSE(broken.prefab.has_value());
    CHECK(broken.message.starts_with("line "));
    CHECK(broken.message.find(", column ") != std::string::npos);

    const PrefabReadResult empty = prefabFromText(zeroRoots);
    CHECK_FALSE(empty.prefab.has_value());
    CHECK(empty.message == "a prefab has exactly one root");

    const PrefabReadResult several = prefabFromText(twoRoots);
    CHECK_FALSE(several.prefab.has_value());
    CHECK(several.message == "a prefab has exactly one root");

    CHECK(log.count() == 0);  // a bridge that returns status and never prints it
}

// ==== the save below the editor (PE9-PE17) ========================================================
// Driven over a real folder: a TempDir that removes itself, the material_edit_test.cpp shape, with bounded disk I/O and
// still no window, no GPU and no ImGui.

namespace {

class TempDir {
public:
    TempDir() {
        std::error_code ec;
        const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
        static int counter = 0;  // doctest runs serially in one process; a plain counter suffices
        dirPath = base / ("aero_prefab_asset_test_" + std::to_string(++counter));
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

private:
    std::filesystem::path dirPath;
};

// A path built from UTF-8 BYTES, never from a narrow std::string (asset_database_test.cpp's AD30 lesson).
[[nodiscard]] std::filesystem::path pathOf(std::string_view utf8) {
    const std::u8string bytes(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size());
    return std::filesystem::path(bytes);
}

[[nodiscard]] std::string readBytes(std::string_view absolutePath) {
    const std::ifstream in(pathOf(absolutePath), std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void writeBytes(std::string_view absolutePath, std::string_view bytes) {
    std::ofstream out(pathOf(absolutePath), std::ios::binary | std::ios::trunc);
    REQUIRE(static_cast<bool>(out));
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

[[nodiscard]] bool existsOnDisk(std::string_view absolutePath) {
    std::error_code ec;
    return std::filesystem::exists(pathOf(absolutePath), ec);
}

void makeDirectory(std::string_view absolutePath) {
    std::error_code ec;
    std::filesystem::create_directories(pathOf(absolutePath), ec);
    REQUIRE_FALSE(static_cast<bool>(ec));
}

// Every name in `directoryAbsolute`, hidden ones and `.aero-tmp` leftovers included.
[[nodiscard]] std::vector<std::string> namesIn(std::string_view directoryAbsolute) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(pathOf(directoryAbsolute), ec)) {
        const std::u8string bytes = entry.path().filename().u8string();
        out.emplace_back(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
    return out;
}

[[nodiscard]] bool endsWith(std::string_view text, std::string_view tail) {
    return text.size() >= tail.size() && text.substr(text.size() - tail.size()) == tail;
}

// One root named `name`, positioned away from the origin so that capture's zeroing of a single root is visible.
[[nodiscard]] Entity addNamedRoot(World& world, std::string_view name) {
    return makeNamed(world, name, Entity{}, Vec3{4.0F, 1.0F, -2.0F});
}

// The tools-OFF arm of a case that needs the write: the save is refused with the unavailable text and `assetsRoot`
// gains nothing. True when the case has nothing more to say, which is how the cases below hold no preprocessor
// conditional.
[[nodiscard]] bool refusedWithoutSceneIo(const World& world, std::span<const Entity> roots,
                                         std::string_view assetsRoot) {
    if (sceneIoAvailable()) {
        return false;
    }
    const PrefabSaveResult refused = savePrefabFile(world, roots, assetsRoot, "");
    CHECK(refused.error == "scene I/O is not available in this build");
    CHECK(refused.relativePath.empty());
    CHECK(namesIn(assetsRoot).empty());
    return true;
}

}  // namespace

TEST_CASE("prefab_asset: uniqueAssetFileName honours its suffix, counts up and folds case (PE9)") {
    const std::vector<std::string_view> none;
    CHECK(uniqueAssetFileName("Crate", ".aeroprefab", none, 64) == "Crate.aeroprefab");
    CHECK(uniqueAssetFileName("Crate", ".other", none, 64) == "Crate.other");  // not a hard-coded .aeromat

    const std::vector<std::string_view> one{"Crate.aeroprefab"};
    CHECK(uniqueAssetFileName("Crate", ".aeroprefab", one, 64) == "Crate-2.aeroprefab");
    const std::vector<std::string_view> two{"Crate.aeroprefab", "Crate-2.aeroprefab"};
    CHECK(uniqueAssetFileName("Crate", ".aeroprefab", two, 64) == "Crate-3.aeroprefab");

    // A name differing only in case is a collision, because the volume may not tell them apart.
    const std::vector<std::string_view> upper{"CRATE.AEROPREFAB"};
    CHECK(uniqueAssetFileName("Crate", ".aeroprefab", upper, 64) == "Crate-2.aeroprefab");
    // A name with another suffix is not.
    const std::vector<std::string_view> sibling{"Crate.aeromat", "Crate.aeroprefab.meta"};
    CHECK(uniqueAssetFileName("Crate", ".aeroprefab", sibling, 64) == "Crate.aeroprefab");

    // Exhaustion is the empty string, and the bound is the caller's.
    CHECK(uniqueAssetFileName("Crate", ".aeroprefab", one, 1).empty());
    CHECK(uniqueAssetFileName("Crate", ".aeroprefab", one, 2) == "Crate-2.aeroprefab");
    CHECK(uniqueAssetFileName("Crate", ".aeroprefab", none, 0).empty());
}

TEST_CASE("prefab_asset: prefabFileStem keeps a name that fits and falls back to Prefab otherwise (PE10)") {
    CHECK(prefabFileStem("Crate") == "Crate");
    CHECK(prefabFileStem("Crate 2") == "Crate 2");
    CHECK(prefabFileStem("a.b") == "a.b");

    CHECK(prefabFileStem("") == "Prefab");
    CHECK(prefabFileStem("a/b") == "Prefab");
    CHECK(prefabFileStem("a\\b") == "Prefab");
    CHECK(prefabFileStem("a:b") == "Prefab");
    CHECK(prefabFileStem("Crate.") == "Prefab");
    CHECK(prefabFileStem(".Crate") == "Prefab");

    // 244 bytes plus the 11-byte suffix is exactly the 255 a file name may hold; 245 fits alone but not with it.
    const std::string fits(244, 'a');
    const std::string alone(245, 'a');
    CHECK(prefabFileStem(fits) == fits);
    CHECK(engine::editor::validateAssetName(alone) == engine::editor::AssetNameRefusal::None);
    CHECK(prefabFileStem(alone) == "Prefab");
}

TEST_CASE("prefab_asset: one root is saved into the chosen folder and reads back with its root at zero (PE11)") {
    const TempDir tmp;
    const std::string assetsRoot = tmp.utf8() + "/assets";
    makeDirectory(assetsRoot + "/props");
    World world;
    const Entity crate = addNamedRoot(world, "Crate");
    static_cast<void>(makeNamed(world, "Lid", crate, Vec3{0.0F, 1.0F, 0.0F}));
    const std::array<Entity, 1> roots{crate};
    if (refusedWithoutSceneIo(world, roots, assetsRoot + "/props")) {
        return;
    }

    const PrefabSaveResult saved = savePrefabFile(world, roots, assetsRoot, "props");
    CHECK(saved.error.empty());
    CHECK(saved.relativePath == "props/Crate.aeroprefab");
    CHECK(saved.relativePath.find('\\') == std::string::npos);
    CHECK(saved.relativePath.front() != '/');
    CHECK(saved.relativePath.find(':') == std::string::npos);  // assets-relative, never absolute

    const std::string text = readBytes(assetsRoot + "/" + saved.relativePath);
    REQUIRE_FALSE(text.empty());
    const PrefabReadResult read = prefabFromText(text);
    REQUIRE(read.prefab.has_value());
    CHECK(read.message.empty());
    CHECK(read.prefab->entityCount() == 2);
    CHECK(read.prefab->world().name(read.prefab->root()) == "Crate");
    const auto* rootTransform = read.prefab->world().get<Transform>(read.prefab->root());
    REQUIRE(rootTransform != nullptr);
    CHECK(rootTransform->position == Vec3{});  // one root is its own pivot, written as the literal zero

    CHECK(namesIn(assetsRoot + "/props") == std::vector<std::string>{"Crate.aeroprefab"});  // no .aero-tmp left
    CHECK_FALSE(existsOnDisk(assetsRoot + "/Crate.aeroprefab"));  // the chosen folder, not the root
}

TEST_CASE("prefab_asset: a taken name is never overwritten and case and folders count as taken (PE12)") {
    const TempDir tmp;
    const std::string assetsRoot = tmp.utf8() + "/assets";
    makeDirectory(assetsRoot);
    World world;
    const Entity crate = addNamedRoot(world, "Crate");
    const std::array<Entity, 1> roots{crate};
    if (refusedWithoutSceneIo(world, roots, assetsRoot)) {
        return;
    }

    writeBytes(assetsRoot + "/Crate.aeroprefab", "keep me");
    PrefabSaveResult saved = savePrefabFile(world, roots, assetsRoot, "");
    CHECK(saved.error.empty());
    CHECK(saved.relativePath == "Crate-2.aeroprefab");
    CHECK(readBytes(assetsRoot + "/Crate.aeroprefab") == "keep me");
    CHECK_FALSE(readBytes(assetsRoot + "/Crate-2.aeroprefab").empty());

    // The listing is unfiltered: a hidden file does not change the answer, a folder owns its name, and a name that
    // differs only in case is taken.
    writeBytes(assetsRoot + "/.keep", "x");
    makeDirectory(assetsRoot + "/Crate-3.aeroprefab");
    writeBytes(assetsRoot + "/CRATE-4.AEROPREFAB", "upper");
    saved = savePrefabFile(world, roots, assetsRoot, "");
    CHECK(saved.error.empty());
    CHECK(saved.relativePath == "Crate-5.aeroprefab");
    CHECK(readBytes(assetsRoot + "/CRATE-4.AEROPREFAB") == "upper");

    // 64 names taken is exhaustion: a refusal that writes nothing.
    for (std::size_t attempt = 6; attempt <= 64; ++attempt) {
        writeBytes(assetsRoot + "/Crate-" + std::to_string(attempt) + ".aeroprefab", "x");
    }
    const std::size_t before = namesIn(assetsRoot).size();
    saved = savePrefabFile(world, roots, assetsRoot, "");
    CHECK(saved.error == "no unused name after 64 attempts");
    CHECK(saved.relativePath.empty());
    CHECK(namesIn(assetsRoot).size() == before);
}

TEST_CASE("prefab_asset: several roots go under a wrapper named for the chosen file (PE13)") {
    const TempDir tmp;
    const std::string assetsRoot = tmp.utf8() + "/assets";
    makeDirectory(assetsRoot);
    World world;
    const Entity crate = addNamedRoot(world, "Crate");
    const Entity barrel = makeNamed(world, "Barrel", Entity{}, Vec3{8.0F, 1.0F, -2.0F});
    const std::array<Entity, 2> roots{crate, barrel};
    if (refusedWithoutSceneIo(world, roots, assetsRoot)) {
        return;
    }

    PrefabSaveResult saved = savePrefabFile(world, roots, assetsRoot, "");
    CHECK(saved.error.empty());
    CHECK(saved.relativePath == "Crate.aeroprefab");
    PrefabReadResult read = prefabFromText(readBytes(assetsRoot + "/" + saved.relativePath));
    REQUIRE(read.prefab.has_value());
    CHECK(read.prefab->entityCount() == 3);
    CHECK(read.prefab->world().name(read.prefab->root()) == "Crate");
    CHECK(childNames(read.prefab->world(), read.prefab->root()) == std::vector<std::string>{"Crate", "Barrel"});

    // The wrapper is named after the file that was chosen, not after the first root.
    saved = savePrefabFile(world, roots, assetsRoot, "");
    CHECK(saved.relativePath == "Crate-2.aeroprefab");
    read = prefabFromText(readBytes(assetsRoot + "/" + saved.relativePath));
    REQUIRE(read.prefab.has_value());
    CHECK(read.prefab->world().name(read.prefab->root()) == "Crate-2");

    // An unnamed first root gives the fallback stem.
    World unnamed;
    const Entity first = unnamed.create();
    const Entity second = makeNamed(unnamed, "Second", Entity{}, Vec3{});
    const std::array<Entity, 2> unnamedRoots{first, second};
    saved = savePrefabFile(unnamed, unnamedRoots, assetsRoot, "");
    CHECK(saved.error.empty());
    CHECK(saved.relativePath == "Prefab.aeroprefab");
    read = prefabFromText(readBytes(assetsRoot + "/" + saved.relativePath));
    REQUIRE(read.prefab.has_value());
    CHECK(read.prefab->world().name(read.prefab->root()) == "Prefab");
}

TEST_CASE("prefab_asset: an empty root, a missing folder and no roots write nothing and say why (PE14)") {
    const TempDir tmp;
    const std::string assetsRoot = tmp.utf8() + "/assets";
    makeDirectory(assetsRoot);
    World world;
    const Entity crate = addNamedRoot(world, "Crate");
    const std::array<Entity, 1> roots{crate};

    // An empty root is refused before anything is concatenated onto it, whatever the build.
    const PrefabSaveResult noProject = savePrefabFile(world, roots, "", "");
    CHECK(noProject.error == "no project is open");
    CHECK(noProject.relativePath.empty());
    CHECK_FALSE(existsOnDisk("/Crate.aeroprefab"));  // the path an empty root would have named

    if (refusedWithoutSceneIo(world, roots, assetsRoot)) {
        return;
    }

    const PrefabSaveResult noFolder = savePrefabFile(world, roots, assetsRoot, "missing");
    CHECK(noFolder.error == "the folder could not be read in full");
    CHECK(noFolder.relativePath.empty());

    const PrefabSaveResult noRoots = savePrefabFile(world, std::span<const Entity>{}, assetsRoot, "");
    CHECK(noRoots.error == "nothing to save");
    CHECK(noRoots.relativePath.empty());

    // A root that died before the drain is refused by capture, with the same words.
    World gone;
    const Entity doomed = addNamedRoot(gone, "Doomed");
    const std::array<Entity, 1> deadRoots{doomed};
    REQUIRE(gone.destroy(doomed));
    const PrefabSaveResult dead = savePrefabFile(gone, deadRoots, assetsRoot, "");
    CHECK(dead.error == "nothing to save");

    CHECK(namesIn(assetsRoot).empty());  // no file, and no .aero-tmp
    CHECK_FALSE(existsOnDisk(assetsRoot + "/missing"));
}

TEST_CASE("prefab_asset: a listing cut short refuses, because a prefix cannot prove a name is free (PE15)") {
    const TempDir tmp;
    const std::string assetsRoot = tmp.utf8() + "/assets";
    makeDirectory(assetsRoot);
    World world;
    const Entity crate = addNamedRoot(world, "Crate");
    const std::array<Entity, 1> roots{crate};
    if (refusedWithoutSceneIo(world, roots, assetsRoot)) {
        return;
    }

    const std::size_t total = engine::editor::MAX_ENTRIES_PER_DIRECTORY + 5;
    for (std::size_t i = 0; i < total; ++i) {
        writeBytes(assetsRoot + "/f" + std::to_string(i), "");
    }
    // The fixture's premise: the listing reads Ok and holds a prefix, which is exactly what `status == Ok` would pass.
    const engine::editor::DirectoryListing listing = engine::editor::listDirectory(assetsRoot, "", true);
    REQUIRE(listing.status == engine::editor::ScanStatus::Ok);
    REQUIRE_FALSE(engine::editor::listingIsComplete(listing));

    const PrefabSaveResult refused = savePrefabFile(world, roots, assetsRoot, "");
    CHECK(refused.error == "the folder could not be read in full");
    CHECK(refused.relativePath.empty());
    CHECK_FALSE(existsOnDisk(assetsRoot + "/Crate.aeroprefab"));
    CHECK(namesIn(assetsRoot).size() == total);
}

TEST_CASE("prefab_asset: a rescan mints one sidecar for a saved prefab and classifies it (PE16)") {
    const TempDir tmp;
    const std::string assetsRoot = tmp.utf8() + "/assets";
    makeDirectory(assetsRoot);
    World world;
    const Entity crate = addNamedRoot(world, "Crate");
    const std::array<Entity, 1> roots{crate};
    if (refusedWithoutSceneIo(world, roots, assetsRoot)) {
        return;
    }
    const PrefabSaveResult saved = savePrefabFile(world, roots, assetsRoot, "");
    REQUIRE(saved.error.empty());

    AssetDatabase database;
    engine::GuidGenerator guids{0x4404U};
    static_cast<void>(database.rescan(tmp.utf8(), assetsRoot, guids));

    const engine::editor::AssetRecord* record = database.findByPath(saved.relativePath);
    REQUIRE(record != nullptr);
    CHECK(record->state == engine::editor::AssetMetaState::Created);
    CHECK(record->guid.valid());
    CHECK(engine::editor::classifyAssetKind(record->relativePath, false) == engine::editor::AssetKind::Prefab);

    std::size_t sidecars = 0;
    for (const std::string& name : namesIn(assetsRoot)) {
        if (endsWith(name, ".meta")) {
            ++sidecars;
        }
    }
    CHECK(sidecars == 1);
    CHECK(existsOnDisk(assetsRoot + "/Crate.aeroprefab.meta"));
}

TEST_CASE(
    "prefab_asset: the save is available exactly when scene I/O is, and refuses without writing otherwise (PE17)") {
    const TempDir tmp;
    const std::string assetsRoot = tmp.utf8() + "/assets";
    makeDirectory(assetsRoot);
    World world;
    const Entity crate = addNamedRoot(world, "Crate");
    const std::array<Entity, 1> roots{crate};

    const PrefabSaveResult result = savePrefabFile(world, roots, assetsRoot, "");
    if (sceneIoAvailable()) {
        CHECK(result.error.empty());
        CHECK(result.relativePath == "Crate.aeroprefab");
        CHECK(existsOnDisk(assetsRoot + "/Crate.aeroprefab"));
    } else {
        CHECK(result.error == "scene I/O is not available in this build");
        CHECK(result.relativePath.empty());
        CHECK(namesIn(assetsRoot).empty());
    }
}
