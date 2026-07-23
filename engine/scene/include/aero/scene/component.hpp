#pragma once
// Aero Engine — component type identity (task 1.3.1).
//
// A component type is identified at runtime by the ADDRESS of a per-type anchor byte. A static data
// member of a class template has vague linkage, so there is exactly one instance per program image
// and &anchor is a collision-free identity with no hashing, no RTTI and no compiler-string parsing.
// Verified across translation units AND across a static archive (the aero_scene shape).
//
// LIMITATION, deliberate and documented: the id is unique per PROGRAM IMAGE. It is never stable
// across process runs and never valid across a shared-library boundary, and it is NEVER serialized.
// The durable identity of a component is its NAME — the reflected fully-qualified name that
// docs/09-file-formats.md uses as a component key. See World::findComponentType().
// If a DLL split ever arrives, the evolution path is a name hash (entt::type_hash is the reference
// implementation) confined to componentTypeId<T>() alone.

#include <type_traits>

namespace engine {

namespace scene::detail {

// One byte per component type, at a unique address. Only the ADDRESS is ever read; the value is
// irrelevant and never inspected.
//
// DELIBERATELY NOT `const` — this is load-bearing, do not "tidy" it. Every instantiation would
// otherwise be a 1-byte, zero-valued, relocation-free READ-ONLY COMDAT, and identical read-only
// COMDATs are exactly what identical-COMDAT-folding merges: MSVC's linker defaults to
// /OPT:REF,ICF,LBR whenever /DEBUG is absent (i.e. every Release link — this repo sets no /OPT
// flags), and Microsoft's own /OPT documentation warns that /OPT:ICF "can cause the same address to
// be assigned to different functions or read-only data members". Folding two anchors would make
// componentTypeId<A>() == componentTypeId<B>() for distinct A and B, so add<B>() would write a B
// into A's storage — type-confused memory corruption. A WRITABLE byte is not an ICF candidate.
// This is LLVM's `static char ID;` pass-identity idiom, and for the same reason.
//
// A compile-time guard cannot catch this (the compiler runs before the linker folds), so the
// runtime guard lives in tests/scene_test.cpp's "distinct component types have distinct ids" case.
template <typename T>
struct ComponentTypeTag {
    static char anchor;
};

template <typename T>
char ComponentTypeTag<T>::anchor = 0;

}  // namespace scene::detail

// Runtime identity of a component type within one program image. Default-constructed == invalid,
// like every other engine identifier.
struct ComponentTypeId {
    const void* value = nullptr;

    [[nodiscard]] constexpr bool valid() const noexcept { return value != nullptr; }
    constexpr bool operator==(const ComponentTypeId&) const noexcept = default;
};

// The id of T. Always valid; identical for the same T everywhere in one program; distinct for
// distinct types. constexpr so compile-time guards (tests/scene_boundary_probe.cpp) can assert it.
template <typename T>
[[nodiscard]] constexpr ComponentTypeId componentTypeId() noexcept {
    static_assert(std::is_same_v<T, std::remove_cvref_t<T>>, "component type must be a plain value type");
    static_assert(std::is_object_v<T>, "a component must be an object type");
    return ComponentTypeId{&scene::detail::ComponentTypeTag<T>::anchor};
}

}  // namespace engine
