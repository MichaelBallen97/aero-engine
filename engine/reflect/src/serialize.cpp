#include <aero/reflect/serialize.hpp>

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

}  // namespace engine::reflect
