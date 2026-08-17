#pragma once

#include "reverse/game_build.hpp"

#include <cstdint>
#include <string>

namespace sc13::reverse {

/** Contains the sole game entry point required by the production M2 runtime patch. */
struct PropDeserializeSymbol final {
    void* address{};
    std::uint32_t rva{};
};

/** Contains exact-build resource-layer entry points resolved from loaded executable bytes. */
struct ResourceSymbols final {
    void* pfRecordReadConstructor{};
    std::uint32_t pfRecordReadConstructorRva{};
    void* publishParsedResource{};
    std::uint32_t publishParsedResourceRva{};
    void* streamRead{};
    std::uint32_t streamReadRva{};
    void* propDeserialize{};
    std::uint32_t propDeserializeRva{};
};

/** Resolves only the critical PROP deserializer and fails on absence or ambiguity. */
[[nodiscard]] bool ResolvePropDeserializeSymbol(
    const BuildFingerprint& fingerprint,
    PropDeserializeSymbol& symbol,
    std::string& error) noexcept;

/** Resolves the complete discovery symbol set when diagnostic tracing is explicitly enabled. */
[[nodiscard]] bool ResolveResourceSymbols(
    const BuildFingerprint& fingerprint,
    ResourceSymbols& symbols,
    std::string& error) noexcept;

}  // namespace sc13::reverse
