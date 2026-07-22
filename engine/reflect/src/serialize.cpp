#include <aero/core/log.hpp>
#include <aero/reflect/serialize.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

namespace engine::reflect {

void writeJson(JsonWriter& w, bool v) { w.value(v); }
void writeJson(JsonWriter& w, float v) { w.value(v); }
void writeJson(JsonWriter& w, double v) { w.value(v); }

void writeJson(JsonWriter& w, const Vec3& v) {
    w.beginObject();
    w.key("x");
    w.value(v.x);
    w.key("y");
    w.value(v.y);
    w.key("z");
    w.value(v.z);
    w.endObject();
}

void writeJson(JsonWriter& w, const Quat& v) {
    w.beginObject();
    w.key("x");
    w.value(v.x);
    w.key("y");
    w.value(v.y);
    w.key("z");
    w.value(v.z);
    w.key("w");
    w.value(v.w);  // x,y,z,w order — F3
    w.endObject();
}

// ---- task 1.2.2: the read half (D10) ---------------------------------------------------------

bool readJson(const JsonValue& v, bool& out) {
    const std::optional<bool> parsed = v.asBool();
    if (!parsed) {
        return false;
    }
    out = *parsed;
    return true;
}

bool readJson(const JsonValue& v, float& out) {
    if (v.isNull()) {  // the writer's non-finite -> null mirror (D10; closes 1.2.1 E-nonfinite)
        out = std::numeric_limits<float>::quiet_NaN();
        return true;
    }
    const std::optional<float> parsed = v.asF32();
    if (!parsed) {
        return false;
    }
    out = *parsed;
    return true;
}

bool readJson(const JsonValue& v, double& out) {
    if (v.isNull()) {
        out = std::numeric_limits<double>::quiet_NaN();
        return true;
    }
    const std::optional<double> parsed = v.asF64();
    if (!parsed) {
        return false;
    }
    out = *parsed;
    return true;
}

// All keys required; each read via the float rule (so "x": null -> NaN mirrors the writer, F7).
// Internally atomic: reads into locals and commits `out` only on success, so a half-good
// {"x":1,"y":"bad","z":3} leaves the caller's field untouched.
bool readJson(const JsonValue& v, Vec3& out) {
    const JsonValue* xv = v.find("x");
    const JsonValue* yv = v.find("y");
    const JsonValue* zv = v.find("z");
    if (xv == nullptr || yv == nullptr || zv == nullptr) {
        return false;
    }
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    if (!readJson(*xv, x) || !readJson(*yv, y) || !readJson(*zv, z)) {
        return false;
    }
    out = Vec3{x, y, z};
    return true;
}

bool readJson(const JsonValue& v, Quat& out) {
    const JsonValue* xv = v.find("x");
    const JsonValue* yv = v.find("y");
    const JsonValue* zv = v.find("z");
    const JsonValue* wv = v.find("w");
    if (xv == nullptr || yv == nullptr || zv == nullptr || wv == nullptr) {
        return false;
    }
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float w = 0.0F;
    if (!readJson(*xv, x) || !readJson(*yv, y) || !readJson(*zv, z) || !readJson(*wv, w)) {
        return false;
    }
    out = Quat{x, y, z, w};
    return true;
}

bool expectObject(const JsonValue& v, std::string_view componentName) {
    if (v.isObject()) {
        return true;
    }
    AERO_LOG_WARN("reflect: {}: expected a JSON object (found {})", componentName, jsonKindName(v.kind()));
    return false;
}

void warnUnknownKeys(const JsonValue& v, std::string_view componentName,
                     std::initializer_list<std::string_view> knownKeys) {
    if (!v.isObject()) {
        return;  // expectObject already warns non-object roots; nothing to scan here
    }
    for (const JsonMember& member : v.members()) {
        bool known = false;
        for (const std::string_view key : knownKeys) {
            if (key == member.key) {
                known = true;
                break;
            }
        }
        if (!known) {
            AERO_LOG_WARN("reflect: {}: ignoring unknown key \"{}\"", componentName, member.key);
        }
    }
}

namespace detail {

void warnBadField(std::string_view componentName, std::string_view fieldName, const JsonValue& v) {
    AERO_LOG_WARN("reflect: {}.{}: cannot read value (found {})", componentName, fieldName, jsonKindName(v.kind()));
}

}  // namespace detail

// ---- task 1.2.3: the generic DOM re-emitter (D9) ------------------------------------------------

namespace {

// One open container while re-emitting: the value being walked plus the index of its next child.
struct EmitFrame {
    const JsonValue* value = nullptr;
    std::size_t index = 0;
};

// Canonicalize a Number lexeme through JsonWriter's TYPED value() overloads -- the writer has no
// raw-lexeme API and stays frozen (spec section 4-F). This classification is a FIXPOINT: its own
// output always re-classifies to the same bytes.
void writeNumberValue(JsonWriter& w, const JsonValue& v) {
    const std::string_view lexeme = v.numberLexeme();
    if (lexeme == "-0") {
        w.value(-0.0);  // sign-preserving: asI64("-0") yields 0 and would drop the '-'
        return;
    }
    // Integral form == no '.', 'e' or 'E' -- exactly the DOM's own rule (json_value.cpp asI64/asU64).
    if (lexeme.find_first_of(".eE") == std::string_view::npos) {
        if (const std::optional<std::int64_t> signedValue = v.asI64()) {
            w.value(static_cast<long long>(*signedValue));  // explicit cast: N1
            return;
        }
        if (const std::optional<std::uint64_t> unsignedValue = v.asU64()) {
            w.value(static_cast<unsigned long long>(*unsignedValue));  // explicit cast: N1
            return;
        }
        // Integral form beyond u64 falls through to the double path (lossy, documented in docs/09).
    }
    if (const std::optional<double> realValue = v.asF64()) {
        w.value(*realValue);
        return;
    }
    w.valueNull();  // rounds to +/-inf (e.g. 1e999): the writer's own non-finite policy, one layer down
}

}  // namespace

void writeJson(JsonWriter& w, const JsonValue& v) {
    std::vector<EmitFrame> stack;
    const JsonValue* pending = &v;
    while (true) {
        if (pending != nullptr) {
            const JsonValue& current = *pending;
            pending = nullptr;
            switch (current.kind()) {
                case JsonKind::Null:
                    w.valueNull();
                    break;
                case JsonKind::Bool:
                    w.value(current.asBool().value_or(false));  // value_or, not *: N2
                    break;
                case JsonKind::Number:
                    writeNumberValue(w, current);
                    break;
                case JsonKind::String:
                    w.value(current.asString().value_or(std::string_view{}));  // N2
                    break;
                case JsonKind::Array:
                    w.beginArray();
                    stack.push_back(EmitFrame{.value = &current, .index = 0});
                    break;
                case JsonKind::Object:
                    w.beginObject();
                    stack.push_back(EmitFrame{.value = &current, .index = 0});
                    break;
            }
            continue;
        }
        if (stack.empty()) {
            return;
        }
        EmitFrame& top = stack.back();
        if (top.value->kind() == JsonKind::Array) {
            const std::vector<JsonValue>& elements = top.value->elements();
            if (top.index < elements.size()) {
                pending = &elements[top.index];
                ++top.index;
                continue;
            }
            w.endArray();
            stack.pop_back();
            continue;
        }
        const std::vector<JsonMember>& members = top.value->members();
        if (top.index < members.size()) {
            const JsonMember& member = members[top.index];
            ++top.index;
            w.key(member.key);
            pending = &member.value;
            continue;
        }
        w.endObject();
        stack.pop_back();
    }
}

}  // namespace engine::reflect
