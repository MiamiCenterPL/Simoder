#include "core/tgi.hpp"

#include <array>
#include <charconv>
#include <cstdio>

namespace sc13::core {
namespace {

/** Parses one hexadecimal TGI component without accepting trailing characters. */
[[nodiscard]] bool ParseComponent(std::string_view text, std::uint32_t& value) noexcept {
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
    }
    if (text.empty() || text.size() > 8) {
        return false;
    }

    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

}  // namespace

std::string ToString(const Tgi& value) {
    std::array<char, 27> buffer{};
    const int length = std::snprintf(
        buffer.data(), buffer.size(), "%08X:%08X:%08X", value.type, value.group, value.instance);
    return length > 0 ? std::string(buffer.data(), static_cast<std::size_t>(length)) : std::string{};
}

std::optional<Tgi> ParseTgi(std::string_view text) noexcept {
    const std::size_t first = text.find(':');
    if (first == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t second = text.find(':', first + 1);
    if (second == std::string_view::npos || text.find(':', second + 1) != std::string_view::npos) {
        return std::nullopt;
    }

    Tgi result{};
    if (!ParseComponent(text.substr(0, first), result.type) ||
        !ParseComponent(text.substr(first + 1, second - first - 1), result.group) ||
        !ParseComponent(text.substr(second + 1), result.instance)) {
        return std::nullopt;
    }
    return result;
}

}  // namespace sc13::core
