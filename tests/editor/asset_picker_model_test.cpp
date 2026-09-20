// tests/editor/asset_picker_model_test.cpp -- task E.3.3: the pure asset-picker model (MP1-MP18).
// A TU of aero_editor_shell_test, which supplies main() from shell_test.cpp -- do NOT define
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN here.
//
// UNGATED (the asset_view_test.cpp / thumbnail_cache_test.cpp precedent): asset_picker_model.hpp
// depends on nothing that needs reflection, so every case here must be PRESENT and PASSING in all
// three build configurations. Tier-0: no GPU, no ImGui, no disk I/O. NO ENTROPY SOURCE ANYWHERE --
// every Guid comes from a fixed-seed GuidGenerator (the standing 3.1.1 rule).
//
// NO `#if` OF ANY KIND anywhere in this file (the standing no-#if-in-a-test-file rule: 3.6.3 shipped
// four cases inside a file-level #if with everything green while the one arm that mattered never ran).
#include <aero/core/guid.hpp>
#include <aero/editor/asset_drag.hpp>
#include <aero/editor/asset_picker_model.hpp>
#include <aero/editor/asset_view.hpp>
#include <aero/editor/thumbnail_cache.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
// MSVC alone needs the COMPLETE std::ostream to stringify a string_view inside a CHECK (the 0.4.1
// trap, hit four times). Included PREVENTIVELY, when the TU was written.
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

using engine::Guid;
using engine::GuidGenerator;
using engine::Vec2;
using engine::editor::AssetFilter;
using engine::editor::AssetKind;
using engine::editor::assetPickerAccepts;
using engine::editor::AssetPickerAnchor;
using engine::editor::assetPickerAnchor;
using engine::editor::AssetPickerCandidates;
using engine::editor::assetPickerFilterOptions;
using engine::editor::assetPickerHeading;
using engine::editor::AssetPickerLayout;
using engine::editor::assetPickerLayout;
using engine::editor::AssetPickerMetrics;
using engine::editor::AssetPickerMove;
using engine::editor::AssetPickerRules;
using engine::editor::assetPickerTruncationNotice;
using engine::editor::AssetRecord;
using engine::editor::buildAssetPickerCandidates;
using engine::editor::classifyAssetDrop;
using engine::editor::DropAction;
using engine::editor::DropSurface;
using engine::editor::initialPickerCursor;
using engine::editor::inspectorAssetFieldKey;
using engine::editor::materialSlotFieldKey;
using engine::editor::movePickerCursor;
using engine::editor::pickerCursorGuid;

namespace {

// THE FIXTURE: nine records in records()' own order -- byte-lexicographic by relativePath -- so the
// walk's "in the database's own order" claim is checkable by reading this table top to bottom.
//
//   a/wood.png      Texture, decodable
//   b/wood.ktx2     Texture, NOT thumbnail-decodable -- the icon-fallback case
//   broken.png      Texture, NIL guid (state Invalid) -- must never be offered
//   hero.glb        Model
//   m/brick.aeromat Material
//   notes.txt       Text -- never offered
//   s/tone.wav      Audio
//   wood/plank.png  Texture -- the leaf-vs-folder trap
//   x.mtl           Unknown -- never offered (.mtl classifies Unknown even though it is importable)
[[nodiscard]] std::vector<AssetRecord> fixtureRecords() {
    GuidGenerator generator{0x0E33E33E33E33E33ULL};  // fixed seed -- NO entropy source anywhere
    const auto make = [&generator](std::string path, bool validIdentity) {
        AssetRecord record;
        record.relativePath = std::move(path);
        record.state = validIdentity ? engine::editor::AssetMetaState::Ok : engine::editor::AssetMetaState::Invalid;
        record.guid = validIdentity ? generator.next() : Guid{};
        record.contentHash = engine::ContentHash{.hi = 0x1122334455667788ULL, .lo = 0x99AABBCCDDEEFF00ULL};
        record.change = engine::editor::ImportChange::UpToDate;
        return record;
    };
    return {make("a/wood.png", true), make("b/wood.ktx2", true),     make("broken.png", false),
            make("hero.glb", true),   make("m/brick.aeromat", true), make("notes.txt", true),
            make("s/tone.wav", true), make("wood/plank.png", true),  make("x.mtl", true)};
}

[[nodiscard]] std::vector<std::string> pathsOf(const AssetPickerCandidates& candidates,
                                               const std::vector<AssetRecord>& records) {
    std::vector<std::string> paths;
    paths.reserve(candidates.items.size());
    for (const engine::editor::AssetPickerCandidate& item : candidates.items) {
        REQUIRE(item.recordIndex < records.size());
        paths.push_back(records[item.recordIndex].relativePath);
    }
    return paths;
}

constexpr AssetPickerRules UNCONSTRAINED_FIELD{DropSurface::AssetField, std::nullopt};
constexpr AssetPickerRules TEXTURE_SLOT{DropSurface::MaterialSlot, std::nullopt};

}  // namespace

TEST_CASE("MP1: the filter options ARE the accepted kinds, in enum order") {
    CHECK(assetPickerFilterOptions(TEXTURE_SLOT) == std::vector<AssetKind>{AssetKind::Texture});
    // ASSET_KIND_FILTER_OPTIONS' order is {Folder, Texture, Model, Audio, Text, Material, Unknown},
    // so the draggable subset comes out Texture, Model, Audio, Material -- never alphabetical, never
    // a list of the model's own.
    CHECK(assetPickerFilterOptions(UNCONSTRAINED_FIELD) ==
          std::vector<AssetKind>{AssetKind::Texture, AssetKind::Model, AssetKind::Audio, AssetKind::Material});
    CHECK(assetPickerFilterOptions(AssetPickerRules{DropSurface::AssetField, AssetKind::Audio}) ==
          std::vector<AssetKind>{AssetKind::Audio});
    CHECK(assetPickerFilterOptions(AssetPickerRules{DropSurface::AssetField, AssetKind::Model}) ==
          std::vector<AssetKind>{AssetKind::Model});
}

TEST_CASE("MP2: the heading is assetKindLabel VERBATIM for one kind -- SINGULAR, never pluralised") {
    CHECK(assetPickerHeading(TEXTURE_SLOT) == "Texture");
    CHECK(assetPickerHeading(AssetPickerRules{DropSurface::AssetField, AssetKind::Texture}) == "Texture");
    CHECK(assetPickerHeading(AssetPickerRules{DropSurface::AssetField, AssetKind::Model}) == "Model");
    // "Audios" is what appending an 's' would produce, which is the reason the rule is "verbatim".
    CHECK(assetPickerHeading(AssetPickerRules{DropSurface::AssetField, AssetKind::Audio}) == "Audio");
    CHECK(assetPickerHeading(AssetPickerRules{DropSurface::AssetField, AssetKind::Material}) == "Material");
    CHECK(assetPickerHeading(UNCONSTRAINED_FIELD) == "Any asset");
    // A field naming a kind no drag can carry accepts nothing at all, and says nothing.
    CHECK(assetPickerHeading(AssetPickerRules{DropSurface::AssetField, AssetKind::Text}).empty());
}

TEST_CASE("MP3: an unconstrained field offers every draggable record, in the database's own order") {
    const std::vector<AssetRecord> records = fixtureRecords();
    AssetPickerCandidates candidates;
    buildAssetPickerCandidates(records, UNCONSTRAINED_FIELD, AssetFilter{}, 100, candidates);

    CHECK(pathsOf(candidates, records) == std::vector<std::string>{"a/wood.png", "b/wood.ktx2", "hero.glb",
                                                                   "m/brick.aeromat", "s/tone.wav", "wood/plank.png"});
    CHECK(candidates.total == 6);
    CHECK_FALSE(candidates.truncated);
    // The three refusals, each for its own reason: a nil guid, a Text record and an Unknown one.
    for (const std::string& path : pathsOf(candidates, records)) {
        CHECK(path != "broken.png");
        CHECK(path != "notes.txt");
        CHECK(path != "x.mtl");
    }
}

TEST_CASE("MP4: the RULES narrow the walk -- a texture slot sees textures, a model field sees models") {
    const std::vector<AssetRecord> records = fixtureRecords();
    AssetPickerCandidates candidates;

    buildAssetPickerCandidates(records, TEXTURE_SLOT, AssetFilter{}, 100, candidates);
    CHECK(pathsOf(candidates, records) == std::vector<std::string>{"a/wood.png", "b/wood.ktx2", "wood/plank.png"});
    CHECK(candidates.total == 3);

    buildAssetPickerCandidates(records, AssetPickerRules{DropSurface::AssetField, AssetKind::Model}, AssetFilter{}, 100,
                               candidates);
    CHECK(pathsOf(candidates, records) == std::vector<std::string>{"hero.glb"});

    buildAssetPickerCandidates(records, AssetPickerRules{DropSurface::AssetField, AssetKind::Audio}, AssetFilter{}, 100,
                               candidates);
    CHECK(pathsOf(candidates, records) == std::vector<std::string>{"s/tone.wav"});

    buildAssetPickerCandidates(records, AssetPickerRules{DropSurface::AssetField, AssetKind::Material}, AssetFilter{},
                               100, candidates);
    CHECK(pathsOf(candidates, records) == std::vector<std::string>{"m/brick.aeromat"});
}

TEST_CASE("MP5: the query matches the LEAF, not the path -- the folder-named-after-the-query trap") {
    const std::vector<AssetRecord> records = fixtureRecords();
    AssetFilter filter;
    filter.query = "wood";
    AssetPickerCandidates candidates;
    buildAssetPickerCandidates(records, UNCONSTRAINED_FIELD, filter, 100, candidates);

    // `wood/plank.png` lives UNDER a directory called wood and must NOT match -- 3.1.3's AV39b
    // species, and the whole reason matchesFilter is composed rather than restated here.
    CHECK(pathsOf(candidates, records) == std::vector<std::string>{"a/wood.png", "b/wood.ktx2"});
    CHECK(candidates.total == 2);
}

TEST_CASE("MP6: the combo narrows WITHIN the rules -- the rules always win") {
    const std::vector<AssetRecord> records = fixtureRecords();
    AssetPickerCandidates candidates;

    AssetFilter audioOnly;
    audioOnly.anyKind = false;
    audioOnly.kind = AssetKind::Audio;
    buildAssetPickerCandidates(records, UNCONSTRAINED_FIELD, audioOnly, 100, candidates);
    CHECK(pathsOf(candidates, records) == std::vector<std::string>{"s/tone.wav"});

    // A combo value the RULES refuse yields nothing at all, rather than widening them.
    AssetFilter textOnly;
    textOnly.anyKind = false;
    textOnly.kind = AssetKind::Text;
    buildAssetPickerCandidates(records, UNCONSTRAINED_FIELD, textOnly, 100, candidates);
    CHECK(candidates.items.empty());
    CHECK(candidates.total == 0);
    CHECK_FALSE(candidates.truncated);
}

TEST_CASE("MP7: `total` is NEVER capped, and the notice names the difference") {
    const std::vector<AssetRecord> records = fixtureRecords();
    AssetPickerCandidates candidates;

    buildAssetPickerCandidates(records, UNCONSTRAINED_FIELD, AssetFilter{}, 2, candidates);
    CHECK(candidates.items.size() == 2);
    CHECK(candidates.total == 6);  // the UNCAPPED match count -- the SearchResult rule
    CHECK(candidates.truncated);
    CHECK(assetPickerTruncationNotice(candidates) == "4 more not shown -- refine the search");

    buildAssetPickerCandidates(records, UNCONSTRAINED_FIELD, AssetFilter{}, 6, candidates);
    CHECK(candidates.items.size() == 6);
    CHECK(candidates.total == 6);
    CHECK_FALSE(candidates.truncated);
    CHECK(assetPickerTruncationNotice(candidates).empty());
}

TEST_CASE("MP8: the walk REUSES its scratch -- a same-shape rebuild moves no allocation") {
    const std::vector<AssetRecord> records = fixtureRecords();
    AssetPickerCandidates candidates;
    buildAssetPickerCandidates(records, UNCONSTRAINED_FIELD, AssetFilter{}, 100, candidates);
    REQUIRE(candidates.items.size() == 6);

    candidates.items.reserve(64);
    const void* data = candidates.items.data();
    const std::size_t capacity = candidates.items.capacity();
    REQUIRE(capacity >= 64);

    buildAssetPickerCandidates(records, UNCONSTRAINED_FIELD, AssetFilter{}, 100, candidates);
    CHECK(candidates.items.size() == 6);
    CHECK(candidates.items.data() == data);
    CHECK(candidates.items.capacity() == capacity);
}

TEST_CASE("MP9: movePickerCursor is TOTAL and CLAMPING -- it never wraps") {
    constexpr std::size_t COUNT = 3;  // the cursor spans [0 (None), 1, 2, 3]
    CHECK(movePickerCursor(0, AssetPickerMove::Next, COUNT) == 1);
    CHECK(movePickerCursor(1, AssetPickerMove::Next, COUNT) == 2);
    CHECK(movePickerCursor(2, AssetPickerMove::Next, COUNT) == 3);
    CHECK(movePickerCursor(3, AssetPickerMove::Next, COUNT) == 3);  // stays at the end, never wraps to 0

    CHECK(movePickerCursor(0, AssetPickerMove::Prev, COUNT) == 0);  // stays at None, never wraps to 3
    CHECK(movePickerCursor(1, AssetPickerMove::Prev, COUNT) == 0);
    CHECK(movePickerCursor(2, AssetPickerMove::Prev, COUNT) == 1);
    CHECK(movePickerCursor(3, AssetPickerMove::Prev, COUNT) == 2);

    CHECK(movePickerCursor(2, AssetPickerMove::First, COUNT) == 0);
    CHECK(movePickerCursor(0, AssetPickerMove::Last, COUNT) == COUNT);

    // An out-of-range cursor is clamped FIRST -- which is what makes a rebuild that shrank the list
    // safe without the caller re-clamping.
    CHECK(movePickerCursor(9, AssetPickerMove::Next, COUNT) == 3);
    CHECK(movePickerCursor(9, AssetPickerMove::Prev, COUNT) == 2);
    CHECK(movePickerCursor(9, AssetPickerMove::Last, COUNT) == 3);

    // An EMPTY list makes every move land on None -- there is nowhere else to be.
    CHECK(movePickerCursor(0, AssetPickerMove::Next, 0) == 0);
    CHECK(movePickerCursor(0, AssetPickerMove::Prev, 0) == 0);
    CHECK(movePickerCursor(5, AssetPickerMove::Last, 0) == 0);
    CHECK(movePickerCursor(5, AssetPickerMove::First, 0) == 0);
}

TEST_CASE("MP10: a fresh open lands on the BOUND asset -- its index PLUS ONE, because 0 is None") {
    const std::vector<AssetRecord> records = fixtureRecords();
    AssetPickerCandidates candidates;
    buildAssetPickerCandidates(records, UNCONSTRAINED_FIELD, AssetFilter{}, 100, candidates);
    REQUIRE(candidates.items.size() == 6);

    for (std::size_t i = 0; i < candidates.items.size(); ++i) {
        CAPTURE(i);
        CHECK(initialPickerCursor(candidates.items[i].guid, candidates) == i + 1);
    }
    // A nil guid is "no reference", which lands on None.
    CHECK(initialPickerCursor(Guid{}, candidates) == 0);
    // And so does a guid this field cannot offer -- `notes.txt`'s, which the walk refused.
    CHECK(initialPickerCursor(records[5].guid, candidates) == 0);
}

TEST_CASE("MP11: committing the cursor means None at 0, the item at i+1, and None past the end") {
    const std::vector<AssetRecord> records = fixtureRecords();
    AssetPickerCandidates candidates;
    buildAssetPickerCandidates(records, UNCONSTRAINED_FIELD, AssetFilter{}, 100, candidates);
    REQUIRE(candidates.items.size() == 6);

    CHECK_FALSE(pickerCursorGuid(0, candidates).has_value());
    for (std::size_t i = 0; i < candidates.items.size(); ++i) {
        CAPTURE(i);
        const std::optional<Guid> guid = pickerCursorGuid(i + 1, candidates);
        REQUIRE(guid.has_value());
        CHECK((*guid == candidates.items[i].guid));
    }
    CHECK_FALSE(pickerCursorGuid(candidates.items.size() + 1, candidates).has_value());
    CHECK_FALSE(pickerCursorGuid(999, candidates).has_value());
}

TEST_CASE("MP12: the anchor keeps the popup on screen -- below, above, and shifted left") {
    const Vec2 workMin{0.0F, 0.0F};
    const Vec2 workMax{1000.0F, 800.0F};
    const Vec2 size{200.0F, 300.0F};

    SUBCASE("room below: directly under the button, no pivot") {
        const AssetPickerAnchor anchor =
            assetPickerAnchor(Vec2{100.0F, 100.0F}, Vec2{300.0F, 120.0F}, size, workMin, workMax);
        CHECK(anchor.pos.x == doctest::Approx(100.0F));
        CHECK(anchor.pos.y == doctest::Approx(120.0F));
        CHECK(anchor.pivot == Vec2{0.0F, 0.0F});
        CHECK_FALSE(anchor.above);
    }
    SUBCASE("no room below, room above: a (0,1) pivot at the button's TOP") {
        const AssetPickerAnchor anchor =
            assetPickerAnchor(Vec2{100.0F, 700.0F}, Vec2{300.0F, 720.0F}, size, workMin, workMax);
        CHECK(anchor.pos.x == doctest::Approx(100.0F));
        CHECK(anchor.pos.y == doctest::Approx(700.0F));
        CHECK(anchor.pivot == Vec2{0.0F, 1.0F});
        CHECK(anchor.above);
    }
    SUBCASE("room in NEITHER direction: stay below, but PINNED to the work area's top") {
        // A button near the bottom of a work area SHORTER than the popup. Flipping into a gap that is
        // also too short would hide the button itself, so it stays below -- but its TOP is pulled onto
        // the work area, because an API-set position is the one ImGui never clamps and a popup placed
        // past the bottom of the screen has its grid child CULLED, which silently submits no tiles at
        // all. (Measured in a 320x180 window: the grid drew on the appearing frame and never again.)
        const AssetPickerAnchor anchor =
            assetPickerAnchor(Vec2{100.0F, 150.0F}, Vec2{300.0F, 170.0F}, size, workMin, Vec2{1000.0F, 200.0F});
        CHECK(anchor.pos.y == doctest::Approx(workMin.y));
        CHECK_FALSE(anchor.above);
    }
    SUBCASE("past the bottom edge: shifted UP so the bottom lands exactly on workMax.y") {
        // The y mirror of the x rule. The geometry is deliberate: a 400-tall work area and a 300-tall
        // popup, with the button at 150..170 -- 170 + 300 overflows the bottom AND 150 - 300 is above
        // the top, so there is room in NEITHER direction and the popup stays below. It still fits once
        // pulled up, which is what separates this arm from the taller-than-the-work-area one.
        const Vec2 shortWork{1000.0F, 400.0F};
        const AssetPickerAnchor anchor =
            assetPickerAnchor(Vec2{100.0F, 150.0F}, Vec2{300.0F, 170.0F}, size, workMin, shortWork);
        CHECK_FALSE(anchor.above);
        CHECK(anchor.pos.y == doctest::Approx(100.0F));
        CHECK(anchor.pos.y + size.y == doctest::Approx(shortWork.y));
    }
    SUBCASE("the flip's pivot is honoured by the y clamp, never clamped as if it were a top-left") {
        // With room above, the (0,1) pivot means the WINDOW's top is pos.y - size.y. Clamping `pos`
        // directly would treat the button's top as the window's top and move a correctly-placed popup.
        const AssetPickerAnchor anchor =
            assetPickerAnchor(Vec2{100.0F, 700.0F}, Vec2{300.0F, 720.0F}, size, workMin, workMax);
        REQUIRE(anchor.above);
        CHECK(anchor.pos.y == doctest::Approx(700.0F));  // unmoved: 700 - 300 = 400 is well inside
    }
    SUBCASE("past the right edge: shifted left so the right edge lands exactly on workMax.x") {
        const AssetPickerAnchor anchor =
            assetPickerAnchor(Vec2{900.0F, 100.0F}, Vec2{980.0F, 120.0F}, size, workMin, workMax);
        CHECK(anchor.pos.x + size.x == doctest::Approx(workMax.x));
        CHECK(anchor.pos.x == doctest::Approx(800.0F));
    }
    SUBCASE("wider than the work area: the LEFT edge, never a negative x") {
        const Vec2 wide{1200.0F, 300.0F};
        const AssetPickerAnchor anchor =
            assetPickerAnchor(Vec2{900.0F, 100.0F}, Vec2{980.0F, 120.0F}, wide, workMin, workMax);
        CHECK(anchor.pos.x == doctest::Approx(workMin.x));
    }
}

TEST_CASE("MP13: a NON-FINITE input returns the below-anchor UNCLAMPED -- no std::clamp on a NaN") {
    const Vec2 workMin{0.0F, 0.0F};
    const Vec2 workMax{1000.0F, 800.0F};
    const Vec2 size{200.0F, 300.0F};
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    // std::clamp(NaN, lo, hi) returns NaN on libc++ (3.7.2's standing rule) and a non-finite window
    // position is an ImGui assertion -- so EVERY input is guarded, not just the obvious one.
    // EXACT, and deliberately not doctest::Approx(...).epsilon(0.0F), which NEVER MATCHES (its
    // comparison is `< 0`) and prints `1 == 1` on failure -- E.1.4's recorded trap. The below-anchor
    // is a VERBATIM copy of two of its inputs, so the claim is bit identity -- and when the input
    // itself is the NaN, the output carries that NaN through, which `==` alone calls unequal.
    const auto sameFloat = [](float lhs, float rhs) { return (std::isnan(lhs) && std::isnan(rhs)) || lhs == rhs; };
    const auto belowUnclamped = [&](Vec2 bMin, Vec2 bMax, Vec2 popup, Vec2 wMin, Vec2 wMax) {
        const AssetPickerAnchor anchor = assetPickerAnchor(bMin, bMax, popup, wMin, wMax);
        CHECK(sameFloat(anchor.pos.x, bMin.x));
        CHECK(sameFloat(anchor.pos.y, bMax.y));
        CHECK(anchor.pivot == Vec2{0.0F, 0.0F});
        CHECK_FALSE(anchor.above);
    };
    belowUnclamped(Vec2{nan, 100.0F}, Vec2{300.0F, 120.0F}, size, workMin, workMax);
    belowUnclamped(Vec2{100.0F, 100.0F}, Vec2{300.0F, nan}, size, workMin, workMax);
    belowUnclamped(Vec2{100.0F, 100.0F}, Vec2{300.0F, 120.0F}, Vec2{nan, 300.0F}, workMin, workMax);
    belowUnclamped(Vec2{100.0F, 100.0F}, Vec2{300.0F, 120.0F}, size, Vec2{nan, 0.0F}, workMax);
    belowUnclamped(Vec2{100.0F, 100.0F}, Vec2{300.0F, 120.0F}, size, workMin, Vec2{nan, 800.0F});
    belowUnclamped(Vec2{100.0F, 100.0F}, Vec2{300.0F, 120.0F}, size, workMin, Vec2{inf, 800.0F});

    // ANTI-VACUITY: the SAME finite inputs really are clamped, so the arms above are a statement
    // about non-finiteness rather than about the anchor doing nothing.
    const AssetPickerAnchor clamped =
        assetPickerAnchor(Vec2{900.0F, 100.0F}, Vec2{980.0F, 120.0F}, size, workMin, workMax);
    CHECK(clamped.pos.x == doctest::Approx(800.0F));
}

TEST_CASE("MP14: assetPickerAccepts IS classifyAssetDrop -- the composition, not a restatement") {
    const std::vector<AssetPickerRules> everyRules = [] {
        std::vector<AssetPickerRules> rules{TEXTURE_SLOT, UNCONSTRAINED_FIELD};
        for (const AssetKind kind : engine::editor::ASSET_KIND_FILTER_OPTIONS) {
            rules.push_back(AssetPickerRules{DropSurface::AssetField, kind});
            rules.push_back(AssetPickerRules{DropSurface::MaterialSlot, kind});
        }
        return rules;
    }();
    REQUIRE(everyRules.size() == 16);

    std::size_t accepted = 0;
    for (const AssetPickerRules& rules : everyRules) {
        for (const AssetKind kind : engine::editor::ASSET_KIND_FILTER_OPTIONS) {
            CAPTURE(static_cast<int>(kind));
            CAPTURE(std::string(engine::editor::dropSurfaceLabel(rules.surface)));
            const bool viaMatrix = classifyAssetDrop(kind, rules.surface, /*targetHasMeshRenderer=*/false,
                                                     rules.fieldKind) != DropAction::None;
            CHECK(assetPickerAccepts(rules, kind) == viaMatrix);
            accepted += viaMatrix ? 1U : 0U;
        }
    }
    // ANTI-VACUITY: the identity is trivially true if BOTH sides always answer false. They do not.
    CHECK(accepted > 0);
}

TEST_CASE("MP15: the walk is over the SPAN it is given -- a record that appeared is seen") {
    std::vector<AssetRecord> records = fixtureRecords();
    AssetPickerCandidates candidates;
    const AssetPickerRules modelField{DropSurface::AssetField, AssetKind::Model};
    buildAssetPickerCandidates(records, modelField, AssetFilter{}, 100, candidates);
    REQUIRE(candidates.items.size() == 1);

    // A rescan added a model. The walk holds no cached copy, so the SAME call with the SAME rules
    // now yields two -- which is what makes the widget's generation-triggered rebuild sufficient.
    GuidGenerator generator{0xABCDEF0123456789ULL};
    AssetRecord added;
    added.relativePath = "zz/second.gltf";
    added.state = engine::editor::AssetMetaState::Ok;
    added.guid = generator.next();
    added.change = engine::editor::ImportChange::UpToDate;
    records.push_back(added);

    buildAssetPickerCandidates(records, modelField, AssetFilter{}, 100, candidates);
    CHECK(pathsOf(candidates, records) == std::vector<std::string>{"hero.glb", "zz/second.gltf"});
    CHECK(candidates.total == 2);
}

TEST_CASE("MP16: a candidate can be LISTED and still have no thumbnail -- the icon fallback's case") {
    const std::vector<AssetRecord> records = fixtureRecords();
    AssetPickerCandidates candidates;
    buildAssetPickerCandidates(records, TEXTURE_SLOT, AssetFilter{}, 100, candidates);
    REQUIRE(candidates.items.size() == 3);

    // b/wood.ktx2 is the second item: a Texture by KIND (so it is offered) that is NOT
    // thumbnail-decodable (so it draws its kind icon). The two predicates are deliberately different
    // tables, and this is the case that says so.
    const engine::editor::AssetPickerCandidate& ktx2 = candidates.items[1];
    CHECK(records[ktx2.recordIndex].relativePath == "b/wood.ktx2");
    CHECK((ktx2.kind == AssetKind::Texture));
    CHECK_FALSE(engine::editor::thumbnailKeyForRecord(records[ktx2.recordIndex]).has_value());

    // ANTI-VACUITY: its decodable neighbour DOES have a key, so the arm above is about the extension.
    const engine::editor::AssetPickerCandidate& png = candidates.items[0];
    CHECK(records[png.recordIndex].relativePath == "a/wood.png");
    CHECK(engine::editor::thumbnailKeyForRecord(records[png.recordIndex]).has_value());
}

TEST_CASE("MP17: the seam keys are spelled ONCE, against literals") {
    // The FULL registration name is what ComponentEntry::name holds, so a short-name key would never
    // match the request the Inspector's own arm builds.
    CHECK(inspectorAssetFieldKey("engine::MeshRenderer", "mesh") == "engine::MeshRenderer.mesh");
    CHECK(inspectorAssetFieldKey("engine::MeshRenderer", "material") == "engine::MeshRenderer.material");
    CHECK(inspectorAssetFieldKey("engine::AudioSource", "clip") == "engine::AudioSource.clip");
    CHECK(materialSlotFieldKey(0) == "slot:0");
    CHECK(materialSlotFieldKey(3) == "slot:3");
    CHECK(materialSlotFieldKey(4) == "slot:4");
}

TEST_CASE("MP18: the layout is at least as tall as its parts, and always finite") {
    const AssetPickerMetrics metrics{.fontSize = 16.0F,
                                     .frameHeight = 22.0F,
                                     .textLineHeight = 18.0F,
                                     .itemSpacingY = 4.0F,
                                     .windowPadding = 8.0F,
                                     .buttonWidth = 120.0F};

    const AssetPickerLayout noCombo = assetPickerLayout(metrics, /*withCombo=*/false);
    const AssetPickerLayout withCombo = assetPickerLayout(metrics, /*withCombo=*/true);

    // The width is the WIDER of the button and the font-unit minimum, so a narrow Inspector still
    // gets a readable popup and a wide one gets a popup that lines up under its button.
    CHECK(noCombo.popupSize.x >= metrics.buttonWidth);
    CHECK(noCombo.popupSize.x >= engine::editor::ASSET_PICKER_MIN_WIDTH_FONT * metrics.fontSize);
    CHECK(noCombo.popupSize.x == doctest::Approx(engine::editor::ASSET_PICKER_MIN_WIDTH_FONT * metrics.fontSize));

    const AssetPickerMetrics wideButton{.fontSize = 16.0F,
                                        .frameHeight = 22.0F,
                                        .textLineHeight = 18.0F,
                                        .itemSpacingY = 4.0F,
                                        .windowPadding = 8.0F,
                                        .buttonWidth = 900.0F};
    CHECK(assetPickerLayout(wideButton, false).popupSize.x == doctest::Approx(900.0F));

    // The combo costs EXACTLY one frame height plus its own spacing gap.
    CHECK(withCombo.popupSize.y - noCombo.popupSize.y == doctest::Approx(metrics.frameHeight + metrics.itemSpacingY));
    CHECK(withCombo.gridHeight == doctest::Approx(noCombo.gridHeight));

    // The popup is taller than the grid alone -- the heading, the search box and the ALWAYS-RESERVED
    // notice line are all inside it.
    CHECK(noCombo.popupSize.y > noCombo.gridHeight);
    CHECK(noCombo.gridHeight > 0.0F);

    // THE NOTICE LINE IS RESERVED WHETHER OR NOT ANYTHING IS TRUNCATED, and this is the arm that can
    // SEE it: the two above cannot, because dropping one text line leaves the popup taller than the
    // grid and leaves the combo delta unchanged. (Found by seeding exactly that and watching this case
    // stay green.) Written from the PARTS LIST rather than from the implementation's own sum: two
    // paddings, a heading row, a search row, the grid, and one text line for the notice.
    CHECK(noCombo.popupSize.y >= (2.0F * metrics.windowPadding) + metrics.frameHeight + metrics.frameHeight +
                                     noCombo.gridHeight + metrics.textLineHeight);
    CHECK(withCombo.popupSize.y >= (2.0F * metrics.windowPadding) + metrics.frameHeight + metrics.frameHeight +
                                       metrics.frameHeight + withCombo.gridHeight + metrics.textLineHeight);

    // A non-finite or non-positive metric yields a FINITE, POSITIVE size: a NaN window size is an
    // ImGui assertion, so the layout must never be the thing that produces one.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (const AssetPickerMetrics& broken :
         {AssetPickerMetrics{.fontSize = nan, .frameHeight = 22.0F, .textLineHeight = 18.0F},
          AssetPickerMetrics{.fontSize = 16.0F, .frameHeight = nan, .textLineHeight = 18.0F},
          AssetPickerMetrics{.fontSize = 16.0F, .frameHeight = 22.0F, .textLineHeight = nan},
          AssetPickerMetrics{.fontSize = 16.0F, .itemSpacingY = nan, .windowPadding = nan},
          AssetPickerMetrics{.fontSize = -4.0F}, AssetPickerMetrics{.fontSize = 16.0F, .buttonWidth = nan}}) {
        const AssetPickerLayout layout = assetPickerLayout(broken, true);
        CHECK(std::isfinite(layout.popupSize.x));
        CHECK(std::isfinite(layout.popupSize.y));
        CHECK(std::isfinite(layout.gridHeight));
        CHECK(layout.popupSize.x > 0.0F);
        CHECK(layout.popupSize.y > 0.0F);
        CHECK(layout.gridHeight > 0.0F);
    }
}
