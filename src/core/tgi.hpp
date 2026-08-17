#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sc13::core {

/** Identifies a SimCity resource by its 32-bit type, group, and instance values. */
struct Tgi {
    std::uint32_t type{};
    std::uint32_t group{};
    std::uint32_t instance{};

    auto operator<=>(const Tgi&) const = default;
};

/** Formats a TGI as eight-digit uppercase hexadecimal components. */
[[nodiscard]] std::string ToString(const Tgi& value);

/** Parses a TGI written as TYPE:GROUP:INSTANCE, with optional 0x prefixes. */
[[nodiscard]] std::optional<Tgi> ParseTgi(std::string_view text) noexcept;

}  // namespace sc13::core
