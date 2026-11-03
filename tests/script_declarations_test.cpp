// Aero Engine -- the TypeScript declarations against a live VM (task 4.2.2; DT1-DT7, since 4.4.1 DT22-DT25, since
// 4.4.2 DT26-DT28, and since 4.4.3 DT29-DT32).
//
// The build joins engine/script/types/prelude.d.ts with one reflect-gen --emit-dts block per built-in component header
// into aero-engine.d.ts. These cases read that file from disk with a deliberately strict reader (the line grammar in
// the plan's C.2: a reader that skipped what it cannot read would make every comparison vacuous, which DT2 exists to
// catch) and compare it, class by class, with the `engine` namespace a real VM installs. A script API change that
// forgets the prelude, or an emitter that disagrees with the binding table, reddens here. With the reflect tools OFF
// the table is empty and the file is the prelude alone; every case still runs and compares what exists.

#include <aero/core/vfs.hpp>
#include <aero/scene/world.hpp>
#include <aero/script/internal/component_binding.hpp>
#include <aero/script/internal/input_names.hpp>
#include <aero/script/script.hpp>

#include "script_binding_support.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <ostream>  // a CHECK on std::string_view needs the complete std::ostream on MSVC (ci-portability.md)
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using engine::script::internal::FieldKind;
using script_test::completion;
using script_test::expectCleanShutdown;
using script_test::makeBuiltinVm;

// ---- the reader (C.2). Iterative: misc-no-recursion. ---------------------------------------------------------------

struct DeclaredMember {
    std::string name;
    std::string type;       // a field's type text, trimmed; empty for a method
    int arity = -1;         // a method's parameter count; -1 for a field
    bool optional = false;  // `name?(...)`: a method a script may define, never a property of the prototype
    bool rest = false;      // the last parameter is a rest parameter (`...types: T[]`); arity still counts it
};
struct DeclaredClass {
    std::string name;
    int constructorArity = -1;
    bool base = false;  // `protected constructor(...)`: a class a script extends, never one it constructs
    std::vector<DeclaredMember> members;  // public members, in file order
};
struct DeclaredUnion {
    std::string name;
    std::vector<std::string> members;  // the string literals, in file order
};
struct DeclaredNamespace {  // a `namespace <ident> {` block nested in the engine namespace (since 4.4.3)
    std::string name;
    std::vector<DeclaredUnion> unions;      // file order
    std::vector<DeclaredMember> functions;  // its `function` lines, file order
};
struct Declarations {
    std::vector<DeclaredClass> classes;     // file order
    std::vector<DeclaredClass> interfaces;  // a plain `interface Name {` that adds members to the class of that name
    std::vector<DeclaredMember> functions;  // the namespace's `function` lines, file order
    std::vector<DeclaredNamespace> namespaces;  // file order
    std::vector<std::string> componentMap;      // keys, file order
    std::string error;                          // non-empty: refused, "line N: <why>"
};

enum class Context : std::uint8_t { Top, Namespace, Map, TypeBody, Class, Interface, Nested };

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

// Whether the last parameter of `(...)` starts with `...`: the text after the last comma at depth 0.
[[nodiscard]] bool endsInRest(std::string_view parenthesised) {
    const std::string_view inside = trimBoth(parenthesised.substr(1, parenthesised.size() - 2));
    std::size_t lastStart = 0;
    int depth = 0;
    for (std::size_t i = 0; i < inside.size(); ++i) {
        if (isOpener(inside[i])) {
            ++depth;
        } else if (isCloser(inside[i])) {
            --depth;
        } else if (inside[i] == ',' && depth == 0) {
            lastStart = i + 1;
        }
    }
    return startsWith(trimBoth(inside.substr(lastStart)), "...");
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
    if (startsWith(rest, "?")) {  // an optional METHOD only: `name?: type` stays outside the grammar
        rest.remove_prefix(1);
        if (rest.empty() || (rest.front() != '(' && rest.front() != '<')) {
            return false;
        }
        member.optional = true;
    }
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
    member.rest = endsInRest(parameters);
    return true;
}

// `type <Ident> = "<ident>"( | "<ident>")*;`: each member a double-quoted identifier, at least one, none repeated.
[[nodiscard]] bool parseUnion(std::string_view line, DeclaredUnion& declared) {
    if (!startsWith(line, "type ") || !endsWith(line, ";")) {
        return false;
    }
    line.remove_prefix(5);
    line.remove_suffix(1);
    const std::string_view name = leadingIdent(line);
    if (name.empty()) {
        return false;
    }
    line.remove_prefix(name.size());
    if (!startsWith(line, " = ")) {
        return false;
    }
    line.remove_prefix(3);
    declared.name = std::string{name};
    declared.members.clear();
    for (;;) {
        if (line.size() < 2 || line.front() != '"') {
            return false;
        }
        const std::size_t close = line.find('"', 1);
        if (close == std::string_view::npos) {
            return false;
        }
        const std::string_view member = line.substr(1, close - 1);
        if (member.empty() || leadingIdent(member).size() != member.size()) {
            return false;
        }
        if (std::find(declared.members.begin(), declared.members.end(), member) != declared.members.end()) {
            return false;
        }
        declared.members.emplace_back(member);
        line.remove_prefix(close + 1);
        if (line.empty()) {
            return true;
        }
        if (!startsWith(line, " | ")) {
            return false;
        }
        line.remove_prefix(3);
    }
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
                    const std::string_view name = leadingIdent(line.substr(10));
                    if (endsWith(line, "{}")) {
                        understood = true;
                    } else if (line.substr(10 + name.size()) == " {") {  // plain: no generic parameters
                        DeclaredClass declared;
                        declared.name = std::string{name};
                        result.interfaces.push_back(declared);
                        context = Context::Interface;
                        understood = true;
                    } else if (endsWith(line, "{")) {
                        context = Context::TypeBody;
                        understood = true;
                    }
                } else if (startsWith(line, "type ") && !leadingIdent(line.substr(5)).empty() && endsWith(line, ";")) {
                    understood = true;
                } else if (startsWith(line, "namespace ") && endsWith(line, " {")) {
                    const std::string_view name = leadingIdent(line.substr(10));
                    if (!name.empty() && line.substr(10 + name.size()) == " {") {
                        const bool seen = std::any_of(result.namespaces.begin(), result.namespaces.end(),
                                                      [&](const DeclaredNamespace& n) { return n.name == name; });
                        if (seen) {
                            refuse("namespace " + std::string{name} + " is declared twice");
                            return result;
                        }
                        DeclaredNamespace declared;
                        declared.name = std::string{name};
                        result.namespaces.push_back(declared);
                        context = Context::Nested;
                        understood = true;
                    }
                } else if (startsWith(line, "function ")) {  // a method form only: a field after `function` is refused
                    DeclaredMember function;
                    if (parseMember(line.substr(9), function) && function.arity >= 0 && !function.optional) {
                        result.functions.push_back(function);
                        understood = true;
                    }
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
            case Context::Interface:
                if (line == "}") {
                    context = Context::Namespace;
                    understood = true;
                } else {
                    DeclaredMember member;
                    if (parseMember(line, member)) {
                        result.interfaces.back().members.push_back(member);
                        understood = true;
                    }
                }
                break;
            case Context::Nested:
                if (line == "}") {
                    context = Context::Namespace;
                    understood = true;
                } else if (startsWith(line, "type ")) {
                    DeclaredUnion declared;
                    if (parseUnion(line, declared)) {
                        result.namespaces.back().unions.push_back(declared);
                        understood = true;
                    }
                } else if (startsWith(line, "function ")) {
                    DeclaredMember function;
                    if (parseMember(line.substr(9), function) && function.arity >= 0 && !function.optional) {
                        result.namespaces.back().functions.push_back(function);
                        understood = true;
                    }
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
                } else if (startsWith(line, "constructor(") || startsWith(line, "private constructor(") ||
                           startsWith(line, "protected constructor(")) {
                    const std::size_t open = line.find('(');
                    const std::string_view fromOpen = line.substr(open);
                    const std::size_t close = matchingClose(fromOpen);
                    if (close != std::string_view::npos && fromOpen.substr(close) == ";" &&
                        current.constructorArity < 0) {
                        current.constructorArity = arityOf(fromOpen.substr(0, close));
                        current.base = startsWith(line, "protected ");
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
// Only the keys whose value has an own `prototype` are classes; a native function (engine.spawn) has none.
constexpr std::string_view RUNTIME_SHAPE_SCRIPT =
    "Object.getOwnPropertyNames(engine).filter((c) => Object.hasOwn(engine[c], 'prototype')).map((c) => {\n"
    "    const p = engine[c].prototype;\n"
    "    const members = Object.getOwnPropertyNames(p).filter((n) => n !== 'constructor').map((n) => {\n"
    "        const d = Object.getOwnPropertyDescriptor(p, n);\n"
    "        return typeof d.value === 'function' ? n + '/' + d.value.length : n;\n"
    "    });\n"
    "    return c + '/' + engine[c].length + ':' + members.join(',');\n"
    "}).join(';')";

// The namespace's functions: the function-valued keys without an own `prototype`, as name/length.
constexpr std::string_view RUNTIME_FUNCTIONS_SCRIPT =
    "Object.getOwnPropertyNames(engine).filter((c) => typeof engine[c] === 'function' && "
    "!Object.hasOwn(engine[c], 'prototype')).map((c) => c + '/' + engine[c].length).join(',')";

// The keys of engine that are neither a class nor a function: the nested namespaces (since 4.4.3).
constexpr std::string_view RUNTIME_OBJECT_KEYS_SCRIPT =
    "Object.getOwnPropertyNames(engine).filter((c) => typeof engine[c] !== 'function').join(',')";

// engine.input's own names, as name/length.
constexpr std::string_view RUNTIME_INPUT_SCRIPT =
    "Object.getOwnPropertyNames(engine.input).map((n) => n + '/' + engine.input[n].length).join(',')";

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

// A class's members as the runtime sees them: the class's own, then the members the same-name plain interface adds.
[[nodiscard]] std::vector<DeclaredMember> declaredMembersOf(const Declarations& declared, std::string_view name) {
    std::vector<DeclaredMember> members;
    if (const DeclaredClass* const cls = findDeclared(declared, name); cls != nullptr) {
        members = cls->members;
    }
    for (const DeclaredClass& added : declared.interfaces) {
        if (added.name == name) {
            members.insert(members.end(), added.members.begin(), added.members.end());
        }
    }
    return members;
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
    // The join's last LF is dropped before file(CONFIGURE) appends its own, so the file ends in exactly one LF: a
    // second one is a blank line at the end of the artifact.
    CHECK(built.back() == '\n');
    REQUIRE(built.size() >= 2U);
    CHECK(built[built.size() - 2U] != '\n');
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
    CHECK(runtime.size() == tableSize + 4U);
    CHECK(findRuntime(runtime, "Vec3") != nullptr);
    CHECK(findRuntime(runtime, "Quat") != nullptr);
    CHECK(findRuntime(runtime, "Entity") != nullptr);
    CHECK(findRuntime(runtime, "Script") != nullptr);
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
        // A base class's members do not live on its prototype: its non-optional members are instance properties and
        // its optional ones are hooks a script defines (DT23 pins the base class's own shape).
        const std::vector<DeclaredMember> mirrored = declaredMembersOf(declared, cls.name);
        std::vector<std::string> declaredMembers;
        declaredMembers.reserve(mirrored.size());
        for (const DeclaredMember& member : mirrored) {
            CHECK((!member.optional || mirror->base));
            if (!mirror->base) {
                declaredMembers.push_back(member.name);
            }
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

TEST_CASE(
    "declarations: ComponentMap keys are exactly the declared classes beyond the vectors, Entity and bases (DT6)") {
    const Declarations declared = parseDeclarations(builtFile());
    REQUIRE(declared.error.empty());

    std::vector<std::string> keys = declared.componentMap;
    std::sort(keys.begin(), keys.end());
    CHECK(std::adjacent_find(keys.begin(), keys.end()) == keys.end());

    std::vector<std::string> expected = declaredNames(declared);
    std::erase_if(expected, [&](const std::string& name) {
        const DeclaredClass* const cls = findDeclared(declared, name);
        return name == "Vec3" || name == "Quat" || name == "Entity" || (cls != nullptr && cls->base);
    });
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
        const std::vector<DeclaredMember> mirrored = declaredMembersOf(declared, cls.name);
        for (const RuntimeMember& member : cls.members) {
            CAPTURE(member.name);
            const auto it = std::find_if(mirrored.begin(), mirrored.end(),
                                         [&](const DeclaredMember& m) { return m.name == member.name; });
            REQUIRE(it != mirrored.end());
            CHECK((it->arity >= 0) == member.isMethod);
            if (member.isMethod) {
                CHECK(it->arity == member.length);
            }
        }
    }
}

TEST_CASE(
    "declarations: the reader reads a base class and an optional method, and refuses the forms beside them (DT22)") {
    const std::string open = "declare namespace engine {\n";
    const std::string sample = open +
                               "    class Base {\n"
                               "        protected constructor();\n"
                               "        readonly host: Thing;\n"
                               "        onTick?(dt: number): void;\n"
                               "        onStop?(): void;\n"
                               "    }\n"
                               "}\n";
    const Declarations parsed = parseDeclarations(sample);
    CHECK(parsed.error.empty());
    REQUIRE(parsed.classes.size() == 1U);
    const DeclaredClass& base = parsed.classes[0];
    CHECK(base.base);
    CHECK(base.constructorArity == 0);
    REQUIRE(base.members.size() == 3U);
    CHECK(base.members[0].name == "host");
    CHECK(base.members[0].arity == -1);
    CHECK_FALSE(base.members[0].optional);
    CHECK(base.members[1].name == "onTick");
    CHECK(base.members[1].optional);
    CHECK(base.members[1].arity == 1);
    CHECK(base.members[2].name == "onStop");
    CHECK(base.members[2].optional);
    CHECK(base.members[2].arity == 0);

    // The other two constructor forms do not mark a class a base.
    const Declarations plain = parseDeclarations(open + "    class A {\n        constructor();\n    }\n" +
                                                 "    class B {\n        private constructor();\n    }\n}\n");
    REQUIRE(plain.error.empty());
    CHECK_FALSE(plain.classes[0].base);
    CHECK_FALSE(plain.classes[1].base);

    // Each refusal names its line: an optional field, a protected member beside the constructor, an optional method
    // with no return type.
    const std::string head = open + "    class A {\n        protected constructor();\n";
    CHECK(refusedAt(head + "        x?: number;\n    }\n}\n", 4));
    CHECK(refusedAt(head + "        protected x: number;\n    }\n}\n", 4));
    CHECK(refusedAt(head + "        onTick?(dt: number);\n    }\n}\n", 4));
}

TEST_CASE(
    "declarations: the built file declares one base class, Script, and the runtime's prototype holds none of it "
    "(DT23)") {
    const Declarations declared = parseDeclarations(builtFile());
    REQUIRE(declared.error.empty());

    std::vector<std::string> bases;
    for (const DeclaredClass& cls : declared.classes) {
        if (cls.base) {
            bases.push_back(cls.name);
        }
    }
    REQUIRE(bases == std::vector<std::string>{"Script"});

    const DeclaredClass* const script = findDeclared(declared, "Script");
    REQUIRE(script != nullptr);
    CHECK(script->constructorArity == 0);
    REQUIRE(script->members.size() == 4U);
    CHECK(script->members[0].name == "entity");
    CHECK(script->members[0].arity == -1);
    CHECK_FALSE(script->members[0].optional);
    CHECK(std::string_view{script->members[0].type} == "Entity");
    const std::vector<std::pair<std::string, int>> hooks{{"onInit", 0}, {"onUpdate", 1}, {"onDestroy", 0}};
    for (std::size_t i = 0; i < hooks.size(); ++i) {
        const DeclaredMember& member = script->members[i + 1U];
        CAPTURE(member.name);
        CHECK(member.name == hooks[i].first);
        CHECK(member.optional);
        CHECK(member.arity == hooks[i].second);
    }

    const std::vector<RuntimeClass> runtime = runtimeClasses();
    const RuntimeClass* const live = findRuntime(runtime, "Script");
    REQUIRE(live != nullptr);
    CHECK(live->constructorLength == 0);
    CHECK(live->members.empty());
}

namespace {

// One module file in memory, for the VM DT24 builds.
class OneFileBackend final : public engine::FileSystemBackend {
public:
    OneFileBackend(std::string name, std::string text) : name(std::move(name)), text(std::move(text)) {}

    [[nodiscard]] bool exists(std::string_view relPath) const override { return relPath == name; }
    [[nodiscard]] std::optional<std::uint64_t> fileSize(std::string_view relPath) const override {
        return relPath == name ? std::optional<std::uint64_t>{text.size()} : std::nullopt;
    }
    [[nodiscard]] std::optional<engine::ByteBuffer> readFile(std::string_view relPath) const override {
        if (relPath != name) {
            return std::nullopt;
        }
        engine::ByteBuffer bytes;
        bytes.reserve(text.size());
        for (const char c : text) {
            bytes.push_back(static_cast<std::byte>(c));
        }
        return bytes;
    }

private:
    std::string name;
    std::string text;
};

}  // namespace

TEST_CASE("declarations: an attached instance holds exactly the declared non-optional members of Script (DT24)") {
    const Declarations declared = parseDeclarations(builtFile());
    REQUIRE(declared.error.empty());
    const DeclaredClass* const script = findDeclared(declared, "Script");
    REQUIRE(script != nullptr);
    std::vector<std::string> required;
    for (const DeclaredMember& member : script->members) {
        if (!member.optional) {
            required.push_back(member.name);
        }
    }

    engine::VirtualFileSystem vfs;
    vfs.mount(std::make_unique<OneFileBackend>(
        "s/x.js", "var X = class extends engine.Script {\n};\nexport {\n  X as default\n};\n"));
    engine::script::VmCreateResult created = engine::script::Vm::create(script_test::patient(), vfs);
    REQUIRE(created.vm.valid());
    engine::script::Vm vm = std::move(created.vm);
    engine::World world;
    const engine::Entity entity = world.create();
    REQUIRE(vm.bindWorld(world) == engine::script::BindStatus::Ok);
    REQUIRE(engine::script::scriptStatusLabel(vm.loadScript("res://s/x.js").status) == "Ok");
    const engine::script::ScriptInstanceResult made = vm.createInstance("res://s/x.js", entity);
    REQUIRE(engine::script::scriptStatusLabel(made.status) == "Ok");
    REQUIRE(engine::script::internal::VmAccess::defineInstanceGlobal(vm, made.instance, "i"));

    CHECK(completion(vm, "Object.getOwnPropertyNames(i).join()") == "entity");
    CHECK(required == std::vector<std::string>{"entity"});
    CHECK(completion(vm, "String(i.entity instanceof engine.Entity)") == "true");
    expectCleanShutdown(vm);
}

TEST_CASE("declarations: a base class's optional methods are exactly the hooks the VM reads (DT25)") {
    const Declarations declared = parseDeclarations(builtFile());
    REQUIRE(declared.error.empty());
    const DeclaredClass* const script = findDeclared(declared, "Script");
    REQUIRE(script != nullptr);

    std::vector<std::pair<std::string, int>> optionals;
    for (const DeclaredMember& member : script->members) {
        if (member.optional) {
            optionals.emplace_back(member.name, member.arity);
        }
    }
    std::vector<std::pair<std::string, int>> hooks;
    for (std::size_t i = 0; i < engine::script::SCRIPT_HOOK_COUNT; ++i) {
        const auto hook = static_cast<engine::script::ScriptHook>(i);
        const int arity = hook == engine::script::ScriptHook::Update ? 1 : 0;
        hooks.emplace_back(std::string{engine::script::scriptHookName(hook)}, arity);
    }
    CHECK(optionals == hooks);

    for (const DeclaredClass& cls : declared.classes) {
        for (const DeclaredMember& member : cls.members) {
            CAPTURE(cls.name);
            CAPTURE(member.name);
            CHECK((member.optional ? cls.base : true));  // no class but a base declares an optional member
        }
    }
}

TEST_CASE("declarations: the reader returns a sample's function lines and refuses the forms beside them (DT26)") {
    const std::string open = "declare namespace engine {\n";
    const Declarations parsed = parseDeclarations(open +
                                                  "    /** One-line doc. */\n"
                                                  "    function none(): Thing;\n"
                                                  "    function one(a: number): boolean;\n"
                                                  "    function two(a: [number, number], b?: Thing): Thing | null;\n"
                                                  "    function many(...types: Holder<Thing>[]): Thing[];\n"
                                                  "    function mixed(first: string, ...rest: number[]): void;\n"
                                                  "}\n");
    CHECK(parsed.error.empty());
    REQUIRE(parsed.functions.size() == 5U);
    CHECK(parsed.functions[0].name == "none");
    CHECK(parsed.functions[0].arity == 0);
    CHECK_FALSE(parsed.functions[0].rest);
    CHECK(parsed.functions[1].name == "one");
    CHECK(parsed.functions[1].arity == 1);
    CHECK(parsed.functions[2].name == "two");
    CHECK(parsed.functions[2].arity == 2);
    CHECK_FALSE(parsed.functions[2].rest);
    CHECK(parsed.functions[3].name == "many");
    CHECK(parsed.functions[3].arity == 1);
    CHECK(parsed.functions[3].rest);
    CHECK(parsed.functions[4].name == "mixed");
    CHECK(parsed.functions[4].arity == 2);
    CHECK(parsed.functions[4].rest);
    CHECK(parsed.classes.empty());

    // Each refusal names its line: a field after `function`, no return type, an optional function, no name.
    CHECK(refusedAt(open + "    function x: number;\n}\n", 2));
    CHECK(refusedAt(open + "    function f()\n}\n", 2));
    CHECK(refusedAt(open + "    function f();\n}\n", 2));
    CHECK(refusedAt(open + "    function f?(): void;\n}\n", 2));
    CHECK(refusedAt(open + "    function (): void;\n}\n", 2));
}

TEST_CASE("declarations: the namespace's function-valued keys are exactly the declared functions (DT27)") {
    const Declarations declared = parseDeclarations(builtFile());
    REQUIRE(declared.error.empty());
    engine::script::Vm vm = makeBuiltinVm();
    const std::string live = completion(vm, RUNTIME_FUNCTIONS_SCRIPT);
    expectCleanShutdown(vm);

    std::vector<std::string> runtimeNames;
    std::vector<int> lengths;
    for (const std::string& entry : splitOn(live, ',')) {
        const std::size_t slash = entry.find('/');
        REQUIRE(slash != std::string::npos);
        runtimeNames.push_back(entry.substr(0, slash));
        lengths.push_back(std::stoi(entry.substr(slash + 1)));
    }
    std::vector<std::string> declaredFunctions;
    declaredFunctions.reserve(declared.functions.size());
    for (const DeclaredMember& function : declared.functions) {
        declaredFunctions.push_back(function.name);
    }
    CHECK(sameSet(runtimeNames, declaredFunctions, "functions"));
    CHECK_FALSE(runtimeNames.empty());
    for (const DeclaredMember& function : declared.functions) {
        CAPTURE(function.name);
        const auto it = std::find(runtimeNames.begin(), runtimeNames.end(), function.name);
        REQUIRE(it != runtimeNames.end());
        const int length = lengths[static_cast<std::size_t>(it - runtimeNames.begin())];
        // A rest parameter adds nothing to a fixed count: the runtime length is at least the declared fixed ones.
        CHECK((function.rest ? function.arity - 1 <= length : function.arity == length));
    }
}

TEST_CASE("declarations: the reader keeps a plain interface's members apart and refuses a bad line in one (DT28)") {
    const std::string open = "declare namespace engine {\n";
    const std::string classFoo = "    class Foo {\n        constructor();\n        x: number;\n    }\n";
    const Declarations parsed = parseDeclarations(open +
                                                  "    interface Foo {\n"
                                                  "        /** One-line doc. */\n"
                                                  "        go(a: number): this;\n"
                                                  "        stop(): void;\n"
                                                  "    }\n" +
                                                  classFoo +
                                                  "    interface Holder<T extends Foo> {\n"
                                                  "        readonly prototype: T;\n"
                                                  "    }\n"
                                                  "}\n");
    CHECK(parsed.error.empty());
    REQUIRE(parsed.interfaces.size() == 1U);  // the generic interface stays unread
    CHECK(parsed.interfaces[0].name == "Foo");
    CHECK(parsed.interfaces[0].constructorArity == -1);
    REQUIRE(parsed.interfaces[0].members.size() == 2U);
    CHECK(parsed.interfaces[0].members[0].name == "go");
    CHECK(parsed.interfaces[0].members[0].arity == 1);
    CHECK(parsed.interfaces[0].members[1].name == "stop");
    REQUIRE(parsed.classes.size() == 1U);
    CHECK(parsed.classes[0].members.size() == 1U);  // the class keeps its own
    CHECK(declaredMembersOf(parsed, "Foo").size() == 3U);
    CHECK(declaredMembersOf(parsed, "Other").empty());

    // A line the member grammar cannot read is refused at its line, and so is an unclosed interface.
    CHECK(refusedAt(open + "    interface Foo {\n        go(;\n    }\n}\n", 3));
    CHECK(refusedAt(open + "    interface Foo {\n        constructor();\n    }\n}\n", 3));
    CHECK(refusedAt(open + "    interface Foo {\n        go(): void;\n", 3));

    // On the built file: every plain interface names a declared class when the table is bound, and Transform's
    // interface members are the live prototype's methods beyond the bound fields, with equal arities.
    const Declarations declared = parseDeclarations(builtFile());
    REQUIRE(declared.error.empty());
    const auto components = engine::script::internal::builtinComponentBindings().components();
    const auto added = std::find_if(declared.interfaces.begin(), declared.interfaces.end(),
                                    [](const DeclaredClass& i) { return i.name == "Transform"; });
    REQUIRE(added != declared.interfaces.end());
    CHECK_FALSE(added->members.empty());
    if (components.empty()) {
        // The reflect tools are OFF: the interface is declared, and neither the class nor the live class exists.
        CHECK(findDeclared(declared, "Transform") == nullptr);
        CHECK(findRuntime(runtimeClasses(), "Transform") == nullptr);
        return;
    }
    for (const DeclaredClass& plain : declared.interfaces) {
        CAPTURE(plain.name);
        CHECK(findDeclared(declared, plain.name) != nullptr);
    }
    const auto bound =
        std::find_if(components.begin(), components.end(), [](const auto& c) { return c.scriptName == "Transform"; });
    REQUIRE(bound != components.end());
    const std::vector<RuntimeClass> runtime = runtimeClasses();
    const RuntimeClass* const live = findRuntime(runtime, "Transform");
    REQUIRE(live != nullptr);
    std::vector<std::string> liveHelpers;
    for (const RuntimeMember& member : live->members) {
        const bool isField = std::any_of(bound->fields.begin(), bound->fields.end(),
                                         [&](const auto& f) { return f.name == member.name; });
        if (!isField) {
            liveHelpers.push_back(member.name);
            const auto it = std::find_if(added->members.begin(), added->members.end(),
                                         [&](const DeclaredMember& m) { return m.name == member.name; });
            REQUIRE(it != added->members.end());
            CHECK(it->arity == member.length);
        }
    }
    std::vector<std::string> interfaceNames;
    for (const DeclaredMember& member : added->members) {
        interfaceNames.push_back(member.name);
    }
    CHECK(sameSet(liveHelpers, interfaceNames, "Transform's helpers"));
}

TEST_CASE("declarations: the reader returns a nested namespace's unions and functions apart from the top ones (DT29)") {
    const std::string open = "declare namespace engine {\n";
    const Declarations parsed =
        parseDeclarations(open +
                          "    function top(): void;\n"
                          "    namespace input {\n"
                          "        /** doc */\n"
                          "        type Key = \"A\" | \"Num1\" | \"Pause\";\n"
                          "        type Two = \"Left\" | \"Right\";\n"
                          "        function keyDown(key: Key): boolean;\n"
                          "        function gamepadAxis(axis: GamepadAxis, slot?: number): number;\n"
                          "    }\n"
                          "}\n");
    CHECK(parsed.error.empty());
    REQUIRE(parsed.functions.size() == 1U);  // the nested functions never leak into the namespace's own list
    CHECK(parsed.functions[0].name == "top");
    REQUIRE(parsed.namespaces.size() == 1U);
    const DeclaredNamespace& input = parsed.namespaces[0];
    CHECK(input.name == "input");
    REQUIRE(input.unions.size() == 2U);
    CHECK(input.unions[0].name == "Key");
    CHECK(input.unions[0].members == std::vector<std::string>{"A", "Num1", "Pause"});
    CHECK(input.unions[1].name == "Two");
    CHECK(input.unions[1].members == std::vector<std::string>{"Left", "Right"});
    REQUIRE(input.functions.size() == 2U);
    CHECK(input.functions[0].name == "keyDown");
    CHECK(input.functions[0].arity == 1);
    CHECK(input.functions[1].name == "gamepadAxis");
    CHECK(input.functions[1].arity == 2);

    // The block may sit anywhere in the namespace and the line after it is read in the namespace again.
    const Declarations after =
        parseDeclarations(open + "    namespace input {\n    }\n    function late(): void;\n}\n");
    CHECK(after.error.empty());
    REQUIRE(after.functions.size() == 1U);
    CHECK(after.functions[0].name == "late");
}

TEST_CASE("declarations: the reader refuses what it cannot read in a nested namespace, naming the line (DT30)") {
    const std::string open = "declare namespace engine {\n    namespace input {\n";
    const std::string close = "    }\n}\n";
    // Each bad line is the third line of the text.
    CHECK(refusedAt(open + "        type K = \"A\" | B;\n" + close, 3));
    CHECK(refusedAt(open + "        type K = \"1A\";\n" + close, 3));
    CHECK(refusedAt(open + "        type K = ;\n" + close, 3));
    CHECK(refusedAt(open + "        type K = \"\";\n" + close, 3));
    CHECK(refusedAt(open + "        type K = \"A\" | \"A\";\n" + close, 3));
    CHECK(refusedAt(open + "        type K = \"A\"\n" + close, 3));
    CHECK(refusedAt(open + "        type K = string;\n" + close, 3));
    CHECK(refusedAt(open + "        type K = \"A\" |\"B\";\n" + close, 3));
    CHECK(refusedAt(open + "        class A {\n        constructor();\n        }\n" + close, 3));
    CHECK(refusedAt(open + "        interface A {\n        }\n" + close, 3));
    CHECK(refusedAt(open + "        namespace deeper {\n        }\n" + close, 3));
    CHECK(refusedAt(open + "        function f();\n" + close, 3));
    CHECK(refusedAt(open + "        function f?(): void;\n" + close, 3));
    CHECK(refusedAt(open + "        const x: number;\n" + close, 3));
    // A second block of the same name is refused at its own line; a distinct name is fine.
    CHECK(refusedAt(open + close.substr(0, 6) + "    namespace input {\n" + close, 4));
    CHECK_FALSE(refusedAt(open + close.substr(0, 6) + "    namespace other {\n" + close, 4));
    // An input that ends inside the nested block.
    const Declarations unclosed = parseDeclarations(open + "        function f(): void;\n");
    CHECK(unclosed.error == "line 3: the input ends inside a block");
}

TEST_CASE(
    "declarations: engine.input's functions and arities are the declared ones, and no other object hangs off "
    "engine (DT31)") {
    const Declarations declared = parseDeclarations(builtFile());
    REQUIRE(declared.error.empty());
    REQUIRE(declared.namespaces.size() == 1U);
    const DeclaredNamespace& input = declared.namespaces[0];
    CHECK(input.name == "input");

    engine::script::Vm vm = makeBuiltinVm();
    const std::string live = completion(vm, RUNTIME_INPUT_SCRIPT);
    const std::string others = completion(vm, RUNTIME_OBJECT_KEYS_SCRIPT);
    expectCleanShutdown(vm);

    std::vector<std::string> runtimeNames;
    std::vector<int> lengths;
    for (const std::string& entry : splitOn(live, ',')) {
        const std::size_t slash = entry.find('/');
        REQUIRE(slash != std::string::npos);
        runtimeNames.push_back(entry.substr(0, slash));
        lengths.push_back(std::stoi(entry.substr(slash + 1)));
    }
    std::vector<std::string> declaredNames;
    declaredNames.reserve(input.functions.size());
    for (const DeclaredMember& function : input.functions) {
        declaredNames.push_back(function.name);
    }
    CHECK_FALSE(runtimeNames.empty());
    CHECK(sameSet(runtimeNames, declaredNames, "engine.input's functions"));
    for (const DeclaredMember& function : input.functions) {
        CAPTURE(function.name);
        const auto it = std::find(runtimeNames.begin(), runtimeNames.end(), function.name);
        REQUIRE(it != runtimeNames.end());
        CHECK(function.arity == lengths[static_cast<std::size_t>(it - runtimeNames.begin())]);
    }

    std::vector<std::string> declaredNamespaces;
    for (const DeclaredNamespace& nested : declared.namespaces) {
        declaredNamespaces.push_back(nested.name);
    }
    CHECK(sameSet(splitOn(others, ','), declaredNamespaces, "engine's non-function keys"));
}

namespace {

template <std::size_t N>
[[nodiscard]] std::vector<std::string> tableMembers(const std::array<std::string_view, N>& table) {
    std::vector<std::string> out;
    out.reserve(N);
    for (const std::string_view name : table) {
        out.emplace_back(name);
    }
    return out;
}

}  // namespace

TEST_CASE("declarations: engine.input's unions list the name tables' names in table order (DT32)") {
    namespace names = engine::script::internal;
    const Declarations declared = parseDeclarations(builtFile());
    REQUIRE(declared.error.empty());
    REQUIRE(declared.namespaces.size() == 1U);
    const std::vector<DeclaredUnion>& unions = declared.namespaces[0].unions;

    const std::vector<std::pair<std::string, std::vector<std::string>>> expected{
        {"Key", tableMembers(names::KEY_NAMES)},
        {"MouseButton", tableMembers(names::MOUSE_BUTTON_NAMES)},
        {"GamepadButton", tableMembers(names::GAMEPAD_BUTTON_NAMES)},
        {"GamepadAxis", tableMembers(names::GAMEPAD_AXIS_NAMES)},
        {"GamepadStick", tableMembers(names::GAMEPAD_STICK_NAMES)}};
    REQUIRE(unions.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CAPTURE(expected[i].first);
        CHECK(unions[i].name == expected[i].first);
        CHECK(unions[i].members == expected[i].second);  // order too: the declarations list the table as written
    }
}
