#include "core/resource_filter.hpp"

namespace sc13::core {

bool ResourceFilter::Matches(const Tgi& tgi) const noexcept {
    return (!type.has_value() || type.value() == tgi.type) &&
           (!group.has_value() || group.value() == tgi.group) &&
           (!instance.has_value() || instance.value() == tgi.instance);
}

}  // namespace sc13::core
