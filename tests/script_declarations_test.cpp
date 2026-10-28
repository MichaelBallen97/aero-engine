// Aero Engine -- the TypeScript declarations against a live VM (task 4.2.2; DT1-DT7).
//
// The build joins engine/script/types/prelude.d.ts with one reflect-gen --emit-dts block per built-in component header
// into aero-engine.d.ts. These cases read that file from disk with a deliberately strict reader (the line grammar in
// the plan's C.2: a reader that skipped what it cannot read would make every comparison vacuous, which DT2 exists to
// catch) and compare it, class by class, with the `engine` namespace a real VM installs. A script API change that
// forgets the prelude, or an emitter that disagrees with the binding table, reddens here. With the reflect tools OFF
// the table is empty and the file is the prelude alone; every case still runs and compares what exists.

#include <aero/script/internal/component_binding.hpp>
#include <aero/script/script.hpp>

#include "script_binding_support.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <ostream>  // a CHECK on std::string_view needs the complete std::ostream on MSVC (ci-portability.md)
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

using engine::script::internal::FieldKind;
using script_test::completion;
using script_test::expectCleanShutdown;
using script_test::makeBuiltinVm;

// ---- the reader (C.2). Iterative: misc-no-recursion. ---------------------------------------------------------------

struct DeclaredMember {
    std::string name;
    std::string type;  // a field's type text, trimmed; empty for a method
    int arity = -1;    // a method's parameter count; -1 for a field
};
struct DeclaredClass {
    std::string name;
    int constructorArity = -1;
    std::vector<DeclaredMember> members;  // public members, in file order
};
struct Declarations {
    std::vector<DeclaredClass> classes;     // file order
    std::vector<std::string> componentMap;  // keys, file order
    std::string error;                      // non-empty: refused, "line N: <why>"
};

enum class Context : std::uint8_t { Top, Namespace, Map, TypeBody, Class };

[[nodiscard]] bool startsWith(std::string_view text, std::string_view prefix) {
    return text.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] bool endsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

[[nodiscard]] std::string_view trimBoth(std::string_view text) {
    while (!text.empty() && text.front() == ' ') {
        text.remove_prefix(1);
    }
    while (!text.empty() && text.back() == ' ') {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] bool isIdentStart(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '$';
}

[[nodiscard]] bool isIdentPart(char c) { return isIdentStart(c) || (c >= '0' && c <= '9'); }

// The identifier at the front of `text`, or empty.
[[nodiscard]] std::string_view leadingIdent(std::string_view text) {
    if (text.empty() || !isIdentStart(text.front())) {
        return {};
    }
    std::size_t length = 1;
    while (length < text.size() && isIdentPart(text[length])) {
        ++length;
    }
    return text.substr(0, length);
}

[[nodiscard]] bool isOpener(char c) { return c == '<' || c == '(' || c == '{' || c == '['; }
[[nodiscard]] bool isCloser(char c) { return c == '>' || c == ')' || c == '}' || c == ']'; }

// `text` starts at an opening bracket. The index one past its matching closer, or npos. All four bracket kinds
// nest together; the grammar has no arrow function, so no `>` is ever anything but a closer.
[[nodiscard]] std::size_t matchingClose(std::string_view text) {
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (isOpener(text[i])) {
            ++depth;
        } else if (isCloser(text[i])) {
            --depth;
            if (depth == 0) {
                return i + 1;
            }
        }
    }
    return std::string_view::npos;
}

// The parameter count of `(...)`: 0 for empty parentheses, otherwise 1 + the commas at depth 0 inside them.
[[nodiscard]] int arityOf(std::string_view parenthesised) {
    const std::string_view inside = trimBoth(parenthesised.substr(1, parenthesised.size() - 2));
    if (inside.empty()) {
        return 0;
    }
    int arity = 1;
    int depth = 0;
    for (const char c : inside) {
        if (isOpener(c)) {
            ++depth;
        } else if (isCloser(c)) {
            --depth;
        } else if (c == ',' && depth == 0) {
            ++arity;
        }
    }
    return arity;
}

// A class-body member line (no leading spaces, not a constructor, not private). False when it is neither a field nor
// a method in the grammar.
[[nodiscard]] bool parseMember(std::string_view line, DeclaredMember& member) {
    std::string_view rest = line;
    if (startsWith(rest, "readonly ")) {
        rest.remove_prefix(9);
    }
    const std::string_view ident = leadingIdent(rest);
    if (ident.empty() || !endsWith(rest, ";")) {
        return false;
    }
    rest.remove_prefix(ident.size());
    rest.remove_suffix(1);  // the ';'
    member.name = std::string{ident};
    if (startsWith(rest, ": ")) {
        member.type = std::string{trimBoth(rest.substr(2))};
        member.arity = -1;
        return !member.type.empty();
    }
    if (!rest.empty() && rest.front() == '<') {  // a generic method: skip `<...>`
        const std::size_t generics = matchingClose(rest);
        if (generics == std::string_view::npos) {
            return false;
        }
        rest.remove_prefix(generics);
    }
    if (rest.empty() || rest.front() != '(') {
        return false;
    }
    const std::size_t params = matchingClose(rest);
    if (params == std::string_view::npos) {
        return false;
    }
    const std::string_view parameters = rest.substr(0, params);
    rest.remove_prefix(params);
    if (!startsWith(rest, ": ") || trimBoth(rest.substr(2)).empty()) {
        return false;
    }
    member.type.clear();
    member.arity = arityOf(parameters);
    return true;
}

[[nodiscard]] Declarations parseDeclarations(std::string_view text) {
    Declarations result;
    Context context = Context::Top;
    int lineNo = 0;
    std::size_t position = 0;
    const auto refuse = [&](const std::string& why) { result.error = "line " + std::to_string(lineNo) + ": " + why; };

    while (position < text.size()) {
        const std::size_t newline = text.find('\n', position);
        const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
        std::string_view line = text.substr(position, end - position);
        position = newline == std::string_view::npos ? text.size() : newline + 1;
        ++lineNo;
        while (!line.empty() && line.front() == ' ') {
            line.remove_prefix(1);
        }
        if (line.empty() || startsWith(line, "//")) {
            continue;
        }
        if (startsWith(line, "/*")) {
            if (line.size() < 4 || !endsWith(line, "*/")) {
                refuse("a comment that does not open and close on one line");
                return result;
            }
            continue;
        }
        bool understood = false;
        switch (context) {
            case Context::Top:
                if (line == "declare namespace engine {") {
                    context = Context::Namespace;
                    understood = true;
                }
                break;
            case Context::Namespace:
                if (line == "}") {
                    context = Context::Top;
                    understood = true;
                } else if (line == "interface ComponentMap {") {
                    context = Context::Map;
                    understood = true;
                } else if (startsWith(line, "interface ") && !leadingIdent(line.substr(10)).empty()) {
                    if (endsWith(line, "{}")) {
                        understood = true;
                    } else if (endsWith(line, "{")) {
                        context = Context::TypeBody;
                        understood = true;
                    }
                } else if (startsWith(line, "type ") && !leadingIdent(line.substr(5)).empty() && endsWith(line, ";")) {
                    understood = true;
                } else if (startsWith(line, "class ") && endsWith(line, " {")) {
                    const std::string_view name = leadingIdent(line.substr(6));
                    if (!name.empty() && line.substr(6 + name.size()) == " {") {
                        const bool seen = std::any_of(result.classes.begin(), result.classes.end(),
                                                      [&](const DeclaredClass& c) { return c.name == name; });
                        if (seen) {
                            refuse("class " + std::string{name} + " is declared twice");
                            return result;
                        }
                        DeclaredClass declared;
                        declared.name = std::string{name};
                        result.classes.push_back(declared);
                        context = Context::Class;
                        understood = true;
                    }
                }
                break;
            case Context::Map:
                if (line == "}") {
                    context = Context::Namespace;
                    understood = true;
                } else {
                    const std::string_view key = leadingIdent(line);
                    if (!key.empty() && line == std::string{key} + ": " + std::string{key} + ";") {
                        result.componentMap.emplace_back(key);
                        understood = true;
                    }
                }
                break;
            case Context::TypeBody:
                if (line == "}") {
                    context = Context::Namespace;
                    understood = true;
                } else if (endsWith(line, ";")) {
                    understood = true;
                }
                break;
            case Context::Class: {
                DeclaredClass& current = result.classes.back();
                if (line == "}") {
                    if (current.constructorArity < 0) {
                        refuse("class " + current.name + " has no constructor line");
                        return result;
                    }
                    context = Context::Namespace;
                    understood = true;
                } else if (startsWith(line, "constructor(") || startsWith(line, "private constructor(")) {
                    const std::size_t open = line.find('(');
                    const std::string_view fromOpen = line.substr(open);
                    const std::size_t close = matchingClose(fromOpen);
                    if (close != std::string_view::npos && fromOpen.substr(close) == ";" &&
                        current.constructorArity < 0) {
                        current.constructorArity = arityOf(fromOpen.substr(0, close));
                        understood = true;
                    }
                } else if (startsWith(line, "private ")) {
                    understood = endsWith(line, ";");
                } else {
                    DeclaredMember member;
                    if (parseMember(line, member)) {
                        current.members.push_back(member);
                        understood = true;
                    }
                }
                break;
            }
        }
        if (!understood) {
            refuse("cannot read '" + std::string{line} + "'");
            return result;
        }
    }
    if (context != Context::Top) {
        refuse("the input ends inside a block");
    }
    return result;
}

[[nodiscard]] bool refusedAt(const std::string& text, int line) {
    const Declarations parsed = parseDeclarations(text);
    return parsed.error.starts_with("line " + std::to_string(line) + ": ");
}

// ---- the files and the runtime's side ------------------------------------------------------------------------------

[[nodiscard]] std::string readBinary(const std::string& path) {
    std::ifstream in{path, std::ios::binary};
    REQUIRE_MESSAGE(in.is_open(), "cannot open " << path);
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::string builtFile() { return readBinary(AERO_SCRIPT_DECLARATIONS_FILE); }

struct RuntimeMember {
    std::string name;
    bool isMethod = false;
    int length = 0;
};
struct RuntimeClass {
    std::string name;
    int constructorLength = 0;
    std::vector<RuntimeMember> members;
};

[[nodiscard]] std::vector<std::string> splitOn(std::string_view text, char separator) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t at = text.find(separator, start);
        const std::size_t stop = at == std::string_view::npos ? text.size() : at;
        if (stop > start) {
            out.emplace_back(text.substr(start, stop - start));
        }
        start = stop + 1;
    }
    return out;
}

// One script reads the namespace through property DESCRIPTORS: `p[n]` would run an accessor with the prototype as its
// receiver, which throws "the receiver is not an engine.X"; a descriptor read runs nothing.
constexpr std::string_view RUNTIME_SHAPE_SCRIPT =
    "Object.getOwnPropertyNames(engine).map((c) => {\n"
    "    const p = engine[c].prototype;\n"
    "    const members = Object.getOwnPropertyNames(p).filter((n) => n !== 'constructor').map((n) => {\n"
    "        const d = Object.getOwnPropertyDescriptor(p, n);\n"
    "        return typeof d.value === 'function' ? n + '/' + d.value.length : n;\n"
    "    });\n"
    "    return c + '/' + engine[c].length + ':' + members.join(',');\n"
    "}).join(';')";

[[nodiscard]] std::vector<RuntimeClass> runtimeClasses() {
    engine::script::Vm vm = makeBuiltinVm();
    const std::string shape = completion(vm, RUNTIME_SHAPE_SCRIPT);
    expectCleanShutdown(vm);

    std::vector<RuntimeClass> out;
    for (const std::string& entry : splitOn(shape, ';')) {
        const std::size_t colon = entry.find(':');
        REQUIRE(colon != std::string::npos);
        const std::string head = entry.substr(0, colon);
        const std::size_t slash = head.find('/');
        REQUIRE(slash != std::string::npos);
        RuntimeClass cls;
        cls.name = head.substr(0, slash);
        cls.constructorLength = std::stoi(head.substr(slash + 1));
        for (const std::string& member : splitOn(std::string_view{entry}.substr(colon + 1), ',')) {
            RuntimeMember parsed;
            const std::size_t memberSlash = member.find('/');
            parsed.isMethod = memberSlash != std::string::npos;
            parsed.name = member.substr(0, memberSlash);
            parsed.length = parsed.isMethod ? std::stoi(member.substr(memberSlash + 1)) : 0;
            cls.members.push_back(parsed);
        }
        out.push_back(cls);
    }
    return out;
}

[[nodiscard]] const DeclaredClass* findDeclared(const Declarations& declared, std::string_view name) {
    const auto it = std::find_if(declared.classes.begin(), declared.classes.end(),
                                 [&](const DeclaredClass& c) { return c.name == name; });
    return it == declared.classes.end() ? nullptr : &*it;
}

[[nodiscard]] const RuntimeClass* findRuntime(const std::vector<RuntimeClass>& classes, std::string_view name) {
    const auto it = std::find_if(classes.begin(), classes.end(), [&](const RuntimeClass& c) { return c.name == name; });
    return it == classes.end() ? nullptr : &*it;
}

// Both sides sorted; on a mismatch the symmetric difference is INFO'd, so the failure names the drift.
[[nodiscard]] bool sameSet(std::vector<std::string> left, std::vector<std::string> right, std::string_view what) {
    std::sort(left.begin(), left.end());
    std::sort(right.begin(), right.end());
    if (left == right) {
        return true;
    }
    std::vector<std::string> onlyLeft;
    std::vector<std::string> onlyRight;
    std::set_difference(left.begin(), left.end(), right.begin(), right.end(), std::back_inserter(onlyLeft));
    std::set_difference(right.begin(), right.end(), left.begin(), left.end(), std::back_inserter(onlyRight));
    std::string report{what};
    report += ": only in the runtime/table:";
    for (const std::string& name : onlyLeft) {
        report += " " + name;
    }
    report += "; only in the declarations:";
    for (const std::string& name : onlyRight) {
        report += " " + name;
    }
    INFO(report);
    return false;
}

[[nodiscard]] std::vector<std::string> declaredNames(const Declarations& declared) {
    std::vector<std::string> out;
    out.reserve(declared.classes.size());
    for (const DeclaredClass& cls : declared.classes) {
        out.push_back(cls.name);
    }
    return out;
}

[[nodiscard]] std::string_view typeTextOf(FieldKind kind) {
    switch (kind) {
        case FieldKind::Float:
        case FieldKind::UInt32:
            return "number";
        case FieldKind::Bool:
            return "boolean";
        case FieldKind::Vec3:
            return "Vec3";
        case FieldKind::Quat:
            return "Quat";
        case FieldKind::Guid:
        case FieldKind::String:
            return "string";
    }
    return "";
}

constexpr std::string_view GENERATED_HEAD = "// GENERATED by aero_reflect_gen --emit-dts -- DO NOT EDIT.";
constexpr std::string_view SOURCE_PREFIX = "// source: ";

// `text` split at LF, every line kept (a trailing LF yields no extra empty line).
[[nodiscard]] std::vector<std::string_view> linesOf(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t position = 0;
    while (position < text.size()) {
        const std::size_t newline = text.find('\n', position);
        const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
        lines.push_back(text.substr(position, end - position));
        position = end + 1;
    }
    return lines;
}

// The header file names the build joined, in AERO_BUILTIN_COMPONENT_HEADERS order (tests/CMakeLists.txt); empty with
// the reflect tools OFF.
[[nodiscard]] std::vector<std::string> expectedSources() {
    std::vector<std::string> sources;
    const std::string_view list = AERO_SCRIPT_DECLARATION_SOURCES;
    std::size_t position = 0;
    while (position < list.size()) {
        const std::size_t comma = list.find(',', position);
        const std::size_t end = comma == std::string_view::npos ? list.size() : comma;
        sources.emplace_back(list.substr(position, end - position));
        position = end + 1;
    }
    return sources;
}

}  // namespace

TEST_CASE("declarations: the build's file is 7-bit text, the prelude first, then the header blocks in order (DT1)") {
    const std::string built = builtFile();
    const std::string prelude = readBinary(AERO_SCRIPT_PRELUDE_FILE);
    REQUIRE_FALSE(built.empty());
    REQUIRE_FALSE(prelude.empty());

    const bool onlyPrintable = std::all_of(built.begin(), built.end(), [](const char c) {
        const auto byte = static_cast<unsigned char>(c);
        return byte == 0x0AU || (byte >= 0x20U && byte <= 0x7EU);
    });
    CHECK(onlyPrintable);
    CHECK(built.find('@') == std::string::npos);

    const Declarations parsed = parseDeclarations(built);
    INFO("error: " << parsed.error);
    CHECK(parsed.error.empty());

    REQUIRE(startsWith(built, prelude));
    const std::string_view after = std::string_view{built}.substr(prelude.size());
    if (engine::script::internal::builtinComponentBindings().components().empty()) {
        CHECK(after.empty());
    } else {
        CHECK(startsWith(after, GENERATED_HEAD));
    }

    // AC-5's "in that order": one block per AERO_BUILTIN_COMPONENT_HEADERS entry, each `// source:` line straight
    // after its head, once each, in the list's order. Every `// source:` line counts, so a stray one fails too.
    const std::vector<std::string> expected = expectedSources();
    CHECK(expected.empty() == engine::script::internal::builtinComponentBindings().components().empty());
    std::size_t heads = 0;
    std::vector<std::string> sourcesAfterHead;
    std::vector<std::string> everySource;
    const std::vector<std::string_view> lines = linesOf(built);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (startsWith(lines[i], SOURCE_PREFIX)) {
            everySource.emplace_back(lines[i].substr(SOURCE_PREFIX.size()));
        }
        if (lines[i] == GENERATED_HEAD) {
            ++heads;
            if (i + 1 < lines.size() && startsWith(lines[i + 1], SOURCE_PREFIX)) {
                sourcesAfterHead.emplace_back(lines[i + 1].substr(SOURCE_PREFIX.size()));
            }
        }
    }
    std::string order = "expected:";
    for (const std::string& name : expected) {
        order += " " + name;
    }
    order += "; in the file:";
    for (const std::string& name : everySource) {
        order += " " + name;
    }
    INFO(order);
    CHECK(heads == expected.size());
    CHECK(everySource == expected);
    CHECK(sourcesAfterHead == expected);
}

TEST_CASE("declarations: the reader returns what a valid sample holds and refuses what it cannot read (DT2)") {
    const std::string sample =
        "// a comment\n"
        "declare namespace engine {\n"
        "    /** One-line doc. */\n"
        "    interface ComponentMap {}\n"
        "    interface Holder<T> {\n"
        "        readonly prototype: T;\n"
        "    }\n"
        "    type Component = ComponentMap[keyof ComponentMap];\n"
        "    interface ComponentMap {\n"
        "        Foo: Foo;\n"
        "    }\n"
        "    class Foo {\n"
        "        private constructor();\n"
        "        private readonly __brand: \"Foo\";\n"
        "        /** Position. */\n"
        "        pos: Vec3;\n"
        "        readonly alive: boolean;\n"
        "        get<T extends Component>(type: Holder<T>, a: [number, number], b?: number): T | null;\n"
        "        reset(): this;\n"
        "    }\n"
        "    class Vec {\n"
        "        constructor(x?: number, y?: number);\n"
        "    }\n"
        "}\n";
    const Declarations parsed = parseDeclarations(sample);
    CHECK(parsed.error.empty());
    CHECK(parsed.componentMap == std::vector<std::string>{"Foo"});
    REQUIRE(parsed.classes.size() == 2U);
    CHECK(parsed.classes[0].name == "Foo");
    CHECK(parsed.classes[0].constructorArity == 0);
    REQUIRE(parsed.classes[0].members.size() == 4U);
    CHECK(parsed.classes[0].members[0].name == "pos");
    CHECK(parsed.classes[0].members[0].type == "Vec3");
    CHECK(parsed.classes[0].members[0].arity == -1);
    CHECK(parsed.classes[0].members[1].name == "alive");
    CHECK(parsed.classes[0].members[1].type == "boolean");
    CHECK(parsed.classes[0].members[2].name == "get");
    CHECK(parsed.classes[0].members[2].type.empty());
    CHECK(parsed.classes[0].members[2].arity == 3);
    CHECK(parsed.classes[0].members[3].name == "reset");
    CHECK(parsed.classes[0].members[3].arity == 0);
    CHECK(parsed.classes[1].name == "Vec");
    CHECK(parsed.classes[1].constructorArity == 2);
    CHECK(parsed.classes[1].members.empty());

    // Each refusal names its 1-based line.
    const std::string open = "declare namespace engine {\n";
    CHECK(refusedAt(open + "    class A {\n        constructor();\n        bogus line\n    }\n}\n", 4));
    CHECK(refusedAt(open + "    /**\n     * two lines\n     */\n}\n", 2));
    CHECK(refusedAt("declare const x: number;\n", 1));
    const std::string classA = "    class A {\n        constructor();\n    }\n";
    CHECK(refusedAt(open + classA + classA + "}\n", 5));
    CHECK(refusedAt(open + "    class A {\n        x: number;\n    }\n}\n", 4));
    CHECK(refusedAt(open, 1));
}

TEST_CASE("declarations: the classes the namespace holds are the classes declared (DT3)") {
    const Declarations declared = parseDeclarations(builtFile());
    REQUIRE(declared.error.empty());
    const std::vector<RuntimeClass> runtime = runtimeClasses();
    const std::size_t tableSize = engine::script::internal::builtinComponentBindings().components().size();

    std::vector<std::string> runtimeNames;
    runtimeNames.reserve(runtime.size());
    for (const RuntimeClass& cls : runtime) {
        runtimeNames.push_back(cls.name);
    }
    CHECK(sameSet(runtimeNames, declaredNames(declared), "classes"));
    CHECK(runtime.size() == tableSize + 3U);
    CHECK(findRuntime(runtime, "Vec3") != nullptr);
    CHECK(findRuntime(runtime, "Quat") != nullptr);
    CHECK(findRuntime(runtime, "Entity") != nullptr);
}

TEST_CASE("declarations: each class declares exactly the public names its prototype holds (DT4)") {
    const Declarations declared = parseDeclarations(builtFile());
    REQUIRE(declared.error.empty());
    const std::vector<RuntimeClass> runtime = runtimeClasses();

    for (const RuntimeClass& cls : runtime) {
        CAPTURE(cls.name);
        const DeclaredClass* const mirror = findDeclared(declared, cls.name);
        REQUIRE(mirror != nullptr);
        std::vector<std::string> runtimeNames;
        runtimeNames.reserve(cls.members.size());
        for (const RuntimeMember& member : cls.members) {
            runtimeNames.push_back(member.name);
        }
        std::vector<std::string> declaredMembers;
        declaredMembers.reserve(mirror->members.size());
        for (const DeclaredMember& member : mirror->members) {
            declaredMembers.push_back(member.name);
        }
        CHECK(sameSet(runtimeNames, declaredMembers, cls.name));
    }
}

TEST_CASE("declarations: every bound field is declared with the type its kind maps to (DT5)") {
    const Declarations declared = parseDeclarations(builtFile());
    REQUIRE(declared.error.empty());

    for (const auto& binding : engine::script::internal::builtinComponentBindings().components()) {
        CAPTURE(binding.scriptName);
        const DeclaredClass* const cls = findDeclared(declared, binding.scriptName);
        REQUIRE(cls != nullptr);
        CHECK(cls->members.size() == binding.fields.size());
        for (const auto& field : binding.fields) {
            CAPTURE(field.name);
            const auto it = std::find_if(cls->members.begin(), cls->members.end(),
                                         [&](const DeclaredMember& m) { return m.name == field.name; });
            REQUIRE(it != cls->members.end());
            CHECK(it->arity == -1);
            CHECK(std::string_view{it->type} == typeTextOf(field.kind));
        }
    }
}

TEST_CASE("declarations: ComponentMap keys are exactly the declared classes beyond Vec3, Quat and Entity (DT6)") {
    const Declarations declared = parseDeclarations(builtFile());
    REQUIRE(declared.error.empty());

    std::vector<std::string> keys = declared.componentMap;
    std::sort(keys.begin(), keys.end());
    CHECK(std::adjacent_find(keys.begin(), keys.end()) == keys.end());

    std::vector<std::string> expected = declaredNames(declared);
    std::erase_if(expected,
                  [](const std::string& name) { return name == "Vec3" || name == "Quat" || name == "Entity"; });
    CHECK(sameSet(keys, expected, "ComponentMap"));
}

TEST_CASE("declarations: declared constructor and method arities equal the runtime's function lengths (DT7)") {
    const Declarations declared = parseDeclarations(builtFile());
    REQUIRE(declared.error.empty());
    const std::vector<RuntimeClass> runtime = runtimeClasses();

    for (const RuntimeClass& cls : runtime) {
        CAPTURE(cls.name);
        const DeclaredClass* const mirror = findDeclared(declared, cls.name);
        REQUIRE(mirror != nullptr);
        CHECK(mirror->constructorArity == cls.constructorLength);
        for (const RuntimeMember& member : cls.members) {
            CAPTURE(member.name);
            const auto it = std::find_if(mirror->members.begin(), mirror->members.end(),
                                         [&](const DeclaredMember& m) { return m.name == member.name; });
            REQUIRE(it != mirror->members.end());
            CHECK((it->arity >= 0) == member.isMethod);
            if (member.isMethod) {
                CHECK(it->arity == member.length);
            }
        }
    }
}
