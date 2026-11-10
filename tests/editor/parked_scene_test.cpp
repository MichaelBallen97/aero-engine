// tests/editor/parked_scene_test.cpp -- task 4.7.2: ParkedScene, the scene a game's play leaves untouched (RS1-RS7).
// A TU of aero_editor_shell_test, which supplies main() from shell_test.cpp. Tier 0: no window, no GPU, no ImGui.
// No #if of any kind: the scene-text arms branch on sceneIoAvailable() at run time.
#include <aero/editor/entity_ops.hpp>
#include <aero/editor/parked_scene.hpp>
#include <aero/editor/scene_session.hpp>
#include <aero/editor/selection.hpp>
#include <aero/scene/scene.hpp>

#include "../world_dump.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <ostream>  // MSVC: a CHECK over a std::string needs the complete std::ostream (the 0.4.1 trap)
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

using engine::Entity;
using engine::World;
using engine::editor::createEntity;
using engine::editor::ParkedScene;
using engine::editor::RootOrder;
using engine::editor::sceneIoAvailable;
using engine::editor::sceneToText;
using engine::editor::seedDefaultScene;
using engine::editor::Selection;

namespace {

// The named handles buildScene() hands back.
struct Named {
    Entity r1;
    Entity c1;
    Entity c2;
    Entity g;
    Entity r2;
    Entity r3;
    Entity tmp;  // created and destroyed: a free slot
};

// Deterministic: called twice it gives two Worlds with identical handles and identical root orders. The root order
// ends as [...seed, R2, R3, R1], which a plain reconcile of the finished World would not give.
Named buildScene(World& world, RootOrder& roots) {
    seedDefaultScene(world);
    Named n;
    n.r1 = createEntity(world, {}, "R1");
    n.c1 = createEntity(world, n.r1, "C1");
    n.c2 = createEntity(world, n.r1, "C2");
    n.g = createEntity(world, n.c2, "G");
    n.r2 = createEntity(world, {}, "R2");
    n.r3 = createEntity(world, {}, "R3");
    n.tmp = createEntity(world, {}, "Tmp");
    world.destroy(n.tmp);
    engine::MeshRenderer mesh;
    mesh.mesh = engine::Guid{11ULL, 12ULL};
    mesh.meshIndex = 3;
    world.add<engine::MeshRenderer>(n.c1, mesh);
    world.add<engine::PointLight>(n.r2, engine::PointLight{});
    world.add<engine::ScriptComponent>(n.g, engine::ScriptComponent{engine::Guid{21ULL, 22ULL}});
    roots.reconcile(world);
    world.setParent(n.r1, n.r2);
    roots.reconcile(world);
    world.setParent(n.r1, Entity{});
    roots.reconcile(world);
    return n;
}

std::vector<Entity> rootsOf(const RootOrder& roots) {
    const std::span<const Entity> view = roots.entities();
    return {view.begin(), view.end()};
}

// The three equalities the park promises: the dump, the built-in values, and (where reflection is built) the text.
void checkSameScene(World& actual, World& twin) {
    CHECK(world_test::dumpWorld(actual) == world_test::dumpWorld(twin));
    CHECK(world_test::firstBuiltinDifference(actual, twin).empty());
    CHECK(world_test::firstBuiltinDifference(twin, actual).empty());
    const std::optional<std::string> left = sceneToText(actual);
    const std::optional<std::string> right = sceneToText(twin);
    if (sceneIoAvailable()) {
        CHECK(left.has_value());
        CHECK((left == right));
    } else {
        CHECK_FALSE(left.has_value());
        CHECK_FALSE(right.has_value());
    }
}

bool nextCreatesEqual(World& actual, World& twin, int count) {
    bool equal = true;
    for (int i = 0; i < count; ++i) {
        equal = (actual.create() == twin.create()) && equal;
    }
    return equal;
}

}  // namespace

TEST_CASE("parked scene: park leaves an identical copy and restore brings the original back (task 4.7.2, RS1)") {
    World live;
    RootOrder roots;
    buildScene(live, roots);
    World twin;
    RootOrder twinRoots;
    buildScene(twin, twinRoots);
    checkSameScene(live, twin);

    ParkedScene parked;
    CHECK_FALSE(parked.parked());
    CHECK(parked.park(live, roots));
    CHECK(parked.parked());
    checkSameScene(live, twin);  // the copy keeps every handle and every value

    Selection selection;
    CHECK(parked.restore(live, roots, selection));
    CHECK_FALSE(parked.parked());
    checkSameScene(live, twin);
    CHECK(rootsOf(roots) == rootsOf(twinRoots));
}

TEST_CASE(
    "parked scene: a play that changes everything leaves the restored scene equal to its twin (task 4.7.2, RS2)") {
    World live;
    RootOrder roots;
    const Named n = buildScene(live, roots);
    World twin;
    RootOrder twinRoots;
    buildScene(twin, twinRoots);

    ParkedScene parked;
    REQUIRE(parked.park(live, roots));

    // The play: every kind of change a game makes.
    live.get<engine::Transform>(n.c1)->position = engine::Vec3{9.0F, 8.0F, 7.0F};
    CHECK(live.setName(n.r1, "Renamed"));
    CHECK(live.setParent(n.c1, n.r2));
    live.add<engine::PointLight>(n.g, engine::PointLight{});
    live.remove<engine::MeshRenderer>(n.c1);
    const Entity s1 = createEntity(live, {}, "S1");  // takes the free slot
    const Entity s2 = createEntity(live, s1, "S2");
    CHECK(s1.index == n.tmp.index);
    CHECK(live.destroy(n.r1));
    CHECK(live.destroy(n.r2));
    const Entity recycled = createEntity(live, {}, "Recycled");
    CHECK(live.alive(s2));
    CHECK(live.alive(recycled));
    CHECK_FALSE(live.alive(n.c1));
    roots.reconcile(live);

    Selection selection;
    CHECK(parked.restore(live, roots, selection));
    checkSameScene(live, twin);
    CHECK(nextCreatesEqual(live, twin, 10));
}

TEST_CASE("parked scene: the root order comes back with the World, not reconciled afterwards (task 4.7.2, RS3)") {
    World live;
    RootOrder roots;
    const Named n = buildScene(live, roots);
    World twin;
    RootOrder twinRoots;
    buildScene(twin, twinRoots);
    const std::vector<Entity> expected = rootsOf(twinRoots);
    REQUIRE(rootsOf(roots) == expected);

    ParkedScene parked;
    REQUIRE(parked.park(live, roots));
    CHECK(live.destroy(n.r2));
    roots.reconcile(live);
    const RootOrder playRoots = roots;
    CHECK(rootsOf(playRoots) != expected);

    Selection selection;
    CHECK(parked.restore(live, roots, selection));
    CHECK(rootsOf(roots) == expected);

    // The control: the play-time order, reconciled against the restored World, puts R2 at the bottom.
    RootOrder reconciled = playRoots;
    reconciled.reconcile(live);
    CHECK(rootsOf(reconciled) != expected);
    REQUIRE(!expected.empty());
    CHECK(rootsOf(reconciled).back() == n.r2);
}

TEST_CASE(
    "parked scene: restore prunes the selection against both Worlds and never bumps the revision "
    "(task 4.7.2, RS4)") {
    World live;
    RootOrder roots;
    const Named n = buildScene(live, roots);
    const Entity a = n.r3;
    const Entity b = n.r2;
    const Entity d = n.g;

    ParkedScene parked;
    REQUIRE(parked.park(live, roots));
    const Entity p = createEntity(live, {}, "P");
    CHECK(live.destroy(b));
    CHECK(live.destroy(d));
    const Entity q = createEntity(live, {}, "Q");
    CHECK(q.index == d.index);  // the premise: Q reuses D's slot under a newer generation
    CHECK(q != d);

    Selection selection;
    selection.set(a);
    selection.add(b);
    selection.add(q);
    selection.add(p);
    CHECK(selection.primary() == p);
    const std::uint64_t revision = selection.revision();

    CHECK(parked.restore(live, roots, selection));
    CHECK(selection.revision() == revision);
    REQUIRE(selection.count() == 1);
    CHECK(selection.entities()[0] == a);
    CHECK(selection.primary() == a);
    CHECK(live.alive(b));
    CHECK(live.alive(d));
    CHECK_FALSE(live.alive(q));
    CHECK_FALSE(live.alive(p));
}

TEST_CASE("parked scene: park while parked and restore with nothing parked change nothing (task 4.7.2, RS5)") {
    World live;
    RootOrder roots;
    const Named n = buildScene(live, roots);
    World twin;
    RootOrder twinRoots;
    buildScene(twin, twinRoots);

    ParkedScene parked;
    Selection selection;
    selection.set(n.r1);
    const std::uint64_t revision = selection.revision();
    CHECK_FALSE(parked.restore(live, roots, selection));
    CHECK_FALSE(parked.parked());
    checkSameScene(live, twin);
    CHECK(rootsOf(roots) == rootsOf(twinRoots));
    CHECK(selection.count() == 1);
    CHECK(selection.primary() == n.r1);
    CHECK(selection.revision() == revision);

    REQUIRE(parked.park(live, roots));
    const Entity spawn = createEntity(live, {}, "Spawn");
    const std::string before = world_test::dumpWorld(live);
    CHECK_FALSE(parked.park(live, roots));  // must not overwrite the parked original with the copy
    CHECK(live.alive(spawn));
    CHECK(world_test::dumpWorld(live) == before);

    CHECK(parked.restore(live, roots, selection));
    checkSameScene(live, twin);
    CHECK_FALSE(live.alive(spawn));
}

TEST_CASE("parked scene: fifty cycles leave the World equal to its twin and survive a move (task 4.7.2, RS6)") {
    World live;
    RootOrder roots;
    const Named n = buildScene(live, roots);
    World twin;
    RootOrder twinRoots;
    buildScene(twin, twinRoots);

    ParkedScene parked;
    Selection selection;
    int mismatches = 0;
    for (int cycle = 0; cycle < 50; ++cycle) {
        REQUIRE(parked.park(live, roots));
        for (int i = 0; i < 20; ++i) {
            const std::string name(64, static_cast<char>('a' + (i % 26)));
            const Entity spawned = createEntity(live, {}, name);
            live.add<engine::PointLight>(spawned, engine::PointLight{});
        }
        CHECK(live.destroy(n.r1));
        roots.reconcile(live);
        if (cycle == 25) {
            ParkedScene moved = std::move(parked);
            parked = ParkedScene{};
            CHECK(moved.restore(live, roots, selection));
        } else {
            CHECK(parked.restore(live, roots, selection));
        }
        if (world_test::dumpWorld(live) != world_test::dumpWorld(twin) || rootsOf(roots) != rootsOf(twinRoots)) {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);
    CHECK_FALSE(parked.parked());
    checkSameScene(live, twin);
    CHECK(nextCreatesEqual(live, twin, 10));
}

// ---- RS7: the holder classification pin (source text) --------------------------------------------------------------
// Every editor member whose type holds an Entity carries exactly one stop marker; the scanner below is the contract.
// Every loop is iterative (misc-no-recursion).
namespace {

namespace fs = std::filesystem;

struct SourceLine {
    std::string raw;
    std::string code;  // comments removed, string and character literal contents removed
};

std::string trimmed(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool startsWith(const std::string& text, const std::string& prefix) {
    return text.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(const std::string& text, char last) { return !text.empty() && text.back() == last; }

bool isIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

std::vector<std::string> identifiersOf(const std::string& text) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < text.size()) {
        if (std::isalpha(static_cast<unsigned char>(text[i])) != 0 || text[i] == '_') {
            std::size_t j = i;
            while (j < text.size() && isIdentChar(text[j])) {
                ++j;
            }
            out.push_back(text.substr(i, j - i));
            i = j;
        } else {
            ++i;
        }
    }
    return out;
}

bool hasWord(const std::string& text, const std::string& word) {
    for (const std::string& id : identifiersOf(text)) {
        if (id == word) {
            return true;
        }
    }
    return false;
}

std::vector<SourceLine> readSource(const fs::path& path) {
    std::vector<SourceLine> lines;
    std::ifstream in(path, std::ios::binary);
    std::string raw;
    bool inBlock = false;
    while (std::getline(in, raw)) {
        if (endsWith(raw, '\r')) {
            raw.pop_back();
        }
        std::string code;
        std::size_t i = 0;
        while (i < raw.size()) {
            const char c = raw[i];
            if (inBlock) {
                if (c == '*' && i + 1 < raw.size() && raw[i + 1] == '/') {
                    inBlock = false;
                    i += 2;
                } else {
                    ++i;
                }
            } else if (c == '/' && i + 1 < raw.size() && raw[i + 1] == '/') {
                break;
            } else if (c == '/' && i + 1 < raw.size() && raw[i + 1] == '*') {
                inBlock = true;
                i += 2;
            } else if (c == '"' && !code.empty() && code.back() == 'R' && i + 1 < raw.size() && raw[i + 1] == '(') {
                const std::size_t close = raw.find(")\"", i + 2);
                code += "\"\"";
                i = (close == std::string::npos) ? raw.size() : close + 2;
            } else if (c == '"' || (c == '\'' && (code.empty() || !isIdentChar(code.back())))) {
                code += c;
                ++i;
                while (i < raw.size() && raw[i] != c) {
                    i += (raw[i] == '\\') ? 2 : 1;
                }
                code += c;
                ++i;
            } else {
                code += c;
                ++i;
            }
        }
        lines.push_back({raw, code});
    }
    return lines;
}

struct ClassInfo {
    std::string name;  // qualified by the open classes: HierarchyPanel::PendingAction
    std::vector<std::string> bases;
};

struct MemberInfo {
    std::string owner;  // the qualified class name
    std::string name;
    std::string type;
    std::size_t file = 0;  // index into the scanned file list
    std::size_t line = 0;  // 0-based
};

struct Scan {
    std::vector<fs::path> files;
    std::vector<std::vector<SourceLine>> sources;
    std::vector<ClassInfo> classes;
    std::vector<MemberInfo> members;
};

// "class A::B" reading T resolves T against A::B::T, then A::T, then T.
bool resolvesTo(const std::set<std::string>& names, const std::string& scope, const std::string& id,
                std::string& found) {
    std::string prefix = scope;
    while (true) {
        const std::string candidate = prefix.empty() ? id : prefix + "::" + id;
        if (names.count(candidate) != 0) {
            found = candidate;
            return true;
        }
        if (prefix.empty()) {
            return false;
        }
        const std::size_t cut = prefix.rfind("::");
        prefix = (cut == std::string::npos) ? std::string{} : prefix.substr(0, cut);
    }
}

// The class a code line opens, if it is a class head: not an enum, `struct|class NAME ...`, ends with `{`, no `;`.
bool classHead(const std::string& code, std::string& name, std::vector<std::string>& bases) {
    const std::string line = trimmed(code);
    if (startsWith(line, "enum") || !endsWith(line, '{') || line.find(';') != std::string::npos) {
        return false;
    }
    const std::vector<std::string> ids = identifiersOf(line);
    std::size_t at = ids.size();
    for (std::size_t i = 0; i + 1 < ids.size(); ++i) {
        if (ids[i] == "struct" || ids[i] == "class") {
            at = i + 1;
            break;
        }
    }
    if (at == ids.size()) {
        return false;
    }
    name = ids[at];
    bases.clear();
    const std::size_t colon = line.find(':');
    if (colon != std::string::npos) {
        for (const std::string& id : identifiersOf(line.substr(colon + 1))) {
            if (id != "public" && id != "protected" && id != "private" && id != "virtual" && id != "final") {
                bases.push_back(id);
            }
        }
    }
    return true;
}

// A data-member declaration line: ends in `;`, no parentheses, not a using / friend / ... line.
bool memberDecl(const std::string& code, std::string& type, std::string& name) {
    std::string line = trimmed(code);
    if (!endsWith(line, ';') || line.find('(') != std::string::npos || line.find(')') != std::string::npos) {
        return false;
    }
    for (const char* skip : {"using", "friend", "static_assert", "enum", "template", "typedef", "return", "public",
                             "protected", "private"}) {
        if (startsWith(line, skip)) {
            return false;
        }
    }
    line.pop_back();
    const std::size_t brace = line.find('{');
    if (brace != std::string::npos) {
        line.erase(brace);
    }
    const std::size_t equals = line.find('=');
    if (equals != std::string::npos) {
        line.erase(equals);
    }
    line = trimmed(line);
    while (endsWith(line, ']')) {
        const std::size_t open = line.rfind('[');
        if (open == std::string::npos) {
            return false;
        }
        line = trimmed(line.erase(open));
    }
    std::size_t end = line.size();
    std::size_t begin = end;
    while (begin > 0 && isIdentChar(line[begin - 1])) {
        --begin;
    }
    if (begin == end) {
        return false;
    }
    name = line.substr(begin, end - begin);
    type = trimmed(line.substr(0, begin));
    return !type.empty();
}

Scan scanEditorHeaders() {
    Scan scan;
    for (const char* dir : {AERO_EDITOR_INCLUDE_DIR "/aero/editor", AERO_EDITOR_SRC_DIR}) {
        std::vector<fs::path> found;
        for (const fs::directory_entry& entry : fs::directory_iterator(dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".hpp") {
                found.push_back(entry.path());
            }
        }
        std::sort(found.begin(), found.end());
        scan.files.insert(scan.files.end(), found.begin(), found.end());
    }
    struct Open {
        std::string name;
        int bodyDepth;
    };
    for (std::size_t f = 0; f < scan.files.size(); ++f) {
        scan.sources.push_back(readSource(scan.files[f]));
        const std::vector<SourceLine>& lines = scan.sources.back();
        std::vector<Open> open;
        int depth = 0;
        for (std::size_t n = 0; n < lines.size(); ++n) {
            const std::string& code = lines[n].code;
            std::string name;
            std::vector<std::string> bases;
            int after = depth;
            for (const char c : code) {
                after += (c == '{') ? 1 : (c == '}') ? -1 : 0;
            }
            if (!open.empty() && depth == open.back().bodyDepth) {
                std::string type;
                std::string member;
                if (memberDecl(code, type, member)) {
                    scan.members.push_back({open.back().name, member, type, f, n});
                }
            }
            if (classHead(code, name, bases)) {
                const std::string qualified = open.empty() ? name : open.back().name + "::" + name;
                open.push_back({qualified, after});
                scan.classes.push_back({qualified, bases});
            }
            depth = after;
            while (!open.empty() && depth < open.back().bodyDepth) {
                open.pop_back();
            }
        }
    }
    return scan;
}

struct Classified {
    std::set<std::string> carriers;
    std::vector<std::size_t> holders;  // indexes into Scan::members
};

Classified classify(const Scan& scan) {
    Classified out;
    std::set<std::string> classNames;
    for (const ClassInfo& info : scan.classes) {
        classNames.insert(info.name);
    }
    bool grew = true;
    while (grew) {
        grew = false;
        for (const ClassInfo& info : scan.classes) {
            if (out.carriers.count(info.name) != 0) {
                continue;
            }
            bool carries = false;
            std::string found;
            for (const std::string& base : info.bases) {
                carries = carries || (resolvesTo(out.carriers, info.name, base, found));
            }
            for (const MemberInfo& m : scan.members) {
                if (m.owner != info.name || endsWith(m.type, '*') || endsWith(m.type, '&')) {
                    continue;
                }
                for (const std::string& id : identifiersOf(m.type)) {
                    carries = carries || id == "Entity" || resolvesTo(out.carriers, info.name, id, found);
                }
            }
            if (carries) {
                out.carriers.insert(info.name);
                grew = true;
            }
        }
    }
    for (std::size_t i = 0; i < scan.members.size(); ++i) {
        const MemberInfo& m = scan.members[i];
        if (endsWith(m.type, '*') || endsWith(m.type, '&')) {
            continue;
        }
        std::string found;
        bool holds = false;
        for (const std::string& id : identifiersOf(m.type)) {
            holds = holds || id == "Entity" || resolvesTo(out.carriers, m.owner, id, found);
        }
        if (holds) {
            out.holders.push_back(i);
        }
    }
    return out;
}

const char* const STOP_MARKERS[] = {"[stop:clear]", "[stop:frame]", "[stop:history]", "[stop:value]"};

std::size_t markerTokens(const std::string& raw, std::string* which = nullptr) {
    std::size_t count = 0;
    for (const char* marker : STOP_MARKERS) {
        for (std::size_t at = raw.find(marker); at != std::string::npos; at = raw.find(marker, at + 1)) {
            ++count;
            if (which != nullptr) {
                *which = marker;
            }
        }
    }
    return count;
}

// The lines of `Class::function(...) {` to its matching brace, as one code string; empty when the definition is not
// there exactly once.
std::string functionBody(const fs::path& path, const std::string& signature) {
    const std::vector<SourceLine> lines = readSource(path);
    std::size_t start = lines.size();
    for (std::size_t n = 0; n < lines.size(); ++n) {
        if (lines[n].code.find(signature) != std::string::npos && endsWith(trimmed(lines[n].code), '{')) {
            if (start != lines.size()) {
                return {};
            }
            start = n;
        }
    }
    std::string body;
    int depth = 0;
    for (std::size_t n = start; n < lines.size(); ++n) {
        body += lines[n].code + "\n";
        for (const char c : lines[n].code) {
            depth += (c == '{') ? 1 : (c == '}') ? -1 : 0;
        }
        if (depth == 0) {
            break;
        }
    }
    return start == lines.size() ? std::string{} : body;
}

}  // namespace

TEST_CASE("parked scene: every editor member that holds an entity says what Stop does with it (task 4.7.2, RS7)") {
    const Scan scan = scanEditorHeaders();
    const Classified classified = classify(scan);

    // Anti-vacuity: the scan found the classes and members it exists to read.
    for (const char* name : {"Selection", "RootOrder", "HierarchyAssetDrop", "HierarchyPanel::PendingAction",
                             "InspectorPanel::EditKey", "InspectorPanel::QuatEditCache", "ParkedScene"}) {
        CAPTURE(name);
        CHECK(classified.carriers.count(name) == 1);
    }
    CHECK(classified.holders.size() >= 60);

    // Every holder carries exactly one marker: on its own line, or (when it has none) a whole-line comment above it.
    std::set<std::pair<std::size_t, std::size_t>> holderLines;
    std::set<std::string> clearSet;
    std::set<std::string> unmarked;
    for (const std::size_t index : classified.holders) {
        const MemberInfo& m = scan.members[index];
        holderLines.insert({m.file, m.line});
        const std::vector<SourceLine>& lines = scan.sources[m.file];
        std::string which;
        std::size_t count = markerTokens(lines[m.line].raw, &which);
        if (count == 0 && m.line > 0 && trimmed(lines[m.line - 1].code).empty()) {
            count = markerTokens(lines[m.line - 1].raw, &which);
        }
        const std::string where =
            scan.files[m.file].filename().string() + ":" + std::to_string(m.line + 1) + " " + m.owner + "::" + m.name;
        if (count != 1) {
            unmarked.insert(where);
        }
        if (count == 1 && which == "[stop:clear]") {
            clearSet.insert(m.owner + "::" + m.name);
        }
    }
    std::string unmarkedList;
    for (const std::string& where : unmarked) {
        unmarkedList += where + "\n";
    }
    CAPTURE(unmarkedList);
    CHECK(unmarked.empty());

    // Every marker sits on a holder's line or the line above one.
    std::size_t stray = 0;
    std::string firstStray;
    for (std::size_t f = 0; f < scan.files.size(); ++f) {
        for (std::size_t n = 0; n < scan.sources[f].size(); ++n) {
            if (markerTokens(scan.sources[f][n].raw) == 0) {
                continue;
            }
            const bool onHolder = holderLines.count({f, n}) != 0 || holderLines.count({f, n + 1}) != 0;
            if (!onHolder) {
                ++stray;
                firstStray = scan.files[f].filename().string() + ":" + std::to_string(n + 1);
            }
        }
    }
    CAPTURE(firstStray);
    CHECK(stray == 0);
    CAPTURE(classified.holders.size());

    // The members Stop resets, and the function that resets each class's.
    const std::set<std::string> expectedClear = {
        "EditorApp::sceneSelection",
        "EditorApp::rootOrder",
        "EditorApp::parkedScene",
        "EditorApp::requestedHierarchyDrop",
        "HierarchyPanel::renaming",
        "HierarchyPanel::rangeAnchor",
        "HierarchyPanel::deferredSelectTarget",
        "HierarchyPanel::revealTarget",
        "HierarchyPanel::pendingBeginRename",
        "HierarchyPanel::pendingAssetDrop",
        "HierarchyPanel::pendingSavePrefab",
        "InspectorPanel::quatCache",
        "InspectorPanel::stringCache",
        "ParkedScene::parkedRoots",
    };
    CHECK(clearSet == expectedClear);

    struct Reset {
        const char* owner;
        const char* signature;
        const char* file;
    };
    const Reset resets[] = {
        {"EditorApp", "EditorApp::restoreAfterPlay(", AERO_EDITOR_SRC_DIR "/editor_app.cpp"},
        {"HierarchyPanel", "HierarchyPanel::clearRetainedEntities(", AERO_EDITOR_SRC_DIR "/hierarchy_panel.cpp"},
        {"InspectorPanel", "InspectorPanel::clearRetainedEntities(", AERO_EDITOR_SRC_DIR "/inspector_panel.cpp"},
        {"ParkedScene", "ParkedScene::restore(", AERO_EDITOR_SRC_DIR "/parked_scene.cpp"},
    };
    std::set<std::string> covered;
    for (const Reset& reset : resets) {
        CAPTURE(reset.signature);
        const std::string body = functionBody(reset.file, reset.signature);
        REQUIRE_FALSE(body.empty());
        covered.insert(reset.owner);
        for (const std::string& member : clearSet) {
            const std::string prefix = std::string(reset.owner) + "::";
            if (startsWith(member, prefix)) {
                CAPTURE(member);
                CHECK(hasWord(body, member.substr(prefix.size())));
            }
        }
    }
    for (const std::string& member : clearSet) {
        CAPTURE(member);
        CHECK(covered.count(member.substr(0, member.find("::"))) == 1);
    }
}
