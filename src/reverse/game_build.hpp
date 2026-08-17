#pragma once

#include "core/sha256.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace sc13::reverse {

/** Captures stable file and loaded-image facts for one SimCity build. */
struct BuildFingerprint {
    std::filesystem::path executablePath;
    std::string fileVersion;
    core::Sha256Digest fileSha256{};
    core::Sha256Digest loadedTextSha256{};
    std::uint16_t machine{};
    std::uint32_t timestamp{};
    std::uint32_t imageSize{};
    std::uint32_t textRva{};
    std::uint32_t textSize{};
};

/** Describes one module observed in the current game process. */
struct ModuleInfo {
    std::filesystem::path path;
    std::uintptr_t base{};
    std::uint32_t size{};
};

/** Inspects and fingerprints the current process executable and loaded .text section. */
[[nodiscard]] bool InspectCurrentBuild(
    BuildFingerprint& fingerprint, std::string& error) noexcept;

/** Enumerates modules loaded in the current process at bootstrap time. */
[[nodiscard]] bool EnumerateCurrentModules(
    std::vector<ModuleInfo>& modules, std::string& error) noexcept;

}  // namespace sc13::reverse
