// Aero Engine — the cooked mesh container v1: labels, accessors and the hostile-input parser
// (task 3.3.1). See cooked_mesh.hpp for the contract and docs/09-file-formats.md section 9 for the
// normative format. NEVER THROWS. NEVER READS A FILE. NEVER LOGS. Allocates nothing before the count
// it is allocating for has been validated against a frozen cap.
#include <aero/assets/cooked_mesh.hpp>

namespace engine::assets {

std::string_view cookedMeshStatusLabel(CookedMeshStatus status) noexcept {
    switch (status) {
        case CookedMeshStatus::Ok:
            return "Ok";
        case CookedMeshStatus::TooSmall:
            return "Too small";
        case CookedMeshStatus::BadMagic:
            return "Bad magic";
        case CookedMeshStatus::UnsupportedVersion:
            return "Unsupported version";
        case CookedMeshStatus::ReservedNotZero:
            return "Reserved field not zero";
        case CookedMeshStatus::SizeMismatch:
            return "Size mismatch";
        case CookedMeshStatus::CapExceeded:
            return "Cap exceeded";
        case CookedMeshStatus::BadTable:
            return "Bad table";
        case CookedMeshStatus::BadRange:
            return "Bad range";
        case CookedMeshStatus::BadLayout:
            return "Bad layout";
    }
    return "Unknown";  // unreachable; the switch has no default so a new enumerator is a -Wswitch error
}

}  // namespace engine::assets
