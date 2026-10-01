#include "mods/mod_types.hpp"

namespace sc13::mods {

const char* ModStateName(ModState state) noexcept {
    switch (state) {
        case ModState::Discovered: return "Discovered";
        case ModState::Inactive: return "Inactive";
        case ModState::Loading: return "Loading";
        case ModState::Active: return "Active";
        case ModState::Unloading: return "Unloading";
        case ModState::Failed: return "Failed";
        case ModState::Missing: return "Missing";
        case ModState::Changed: return "Changed";
    }
    return "Unknown";
}

const char* PatchOperationName(PatchOperation operation) noexcept {
    switch (operation) {
        case PatchOperation::Set: return "set";
        case PatchOperation::Add: return "add";
        case PatchOperation::Subtract: return "subtract";
        case PatchOperation::Multiply: return "multiply";
        case PatchOperation::Divide: return "divide";
    }
    return "unknown";
}

}  // namespace sc13::mods
