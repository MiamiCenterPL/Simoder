#pragma once

#include <string>

namespace sc13::mods {
class ModManager;
}

namespace sc13::ui {

/** Binds the process-lifetime UI view model before graphics hooks are created. */
void ConfigureSimoderOverlay(mods::ModManager& manager) noexcept;

/** Creates Direct3D 9 overlay hooks without enabling MinHook globally. */
[[nodiscard]] bool CreateSimoderOverlayHooks(std::string& error) noexcept;

/** Shuts down ImGui and restores the game window procedure after hooks are disabled. */
void ShutdownSimoderOverlay() noexcept;

}  // namespace sc13::ui
