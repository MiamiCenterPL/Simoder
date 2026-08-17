#pragma once

#include <Windows.h>

#include <filesystem>
#include <string>

namespace sc13::runtime {

/** Starts a background observer that captures the executable .text section when it changes. */
[[nodiscard]] bool StartImageObserver(
    HMODULE executableModule,
    const std::filesystem::path& captureDirectory,
    std::string& error) noexcept;

/** Stops the executable image observer and joins its worker thread. */
void StopImageObserver() noexcept;

}  // namespace sc13::runtime
