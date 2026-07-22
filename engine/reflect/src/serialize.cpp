#include <aero/core/log.hpp>
#include <aero/reflect/serialize.hpp>

#include <limits>
#include <optional>

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

}  // namespace engine::reflect
