#pragma once

#include "core/tgi.hpp"

#include <cstdint>
#include <optional>

namespace sc13::core {

/** Selects resources by any optional subset of their semantic TGI fields. */
struct ResourceFilter final {
    std::optional<std::uint32_t> type;
    std::optional<std::uint32_t> group;
    std::optional<std::uint32_t> instance;

    /** Returns true when every configured field equals the supplied resource identity. */
    [[nodiscard]] bool Matches(const Tgi& tgi) const noexcept;
};

}  // namespace sc13::core
