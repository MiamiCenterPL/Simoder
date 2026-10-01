#pragma once

#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

namespace sc13::core {
/** @summary Quotes UTF-8 text for JSON, escaping every ASCII control character. */
[[nodiscard]] inline std::string JsonString(std::string_view text) {
    std::string result = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char character : text) {
        if (character == '"' || character == '\\') { result += '\\'; result += static_cast<char>(character); }
        else if (character < 32U) {
            result += "\\u00"; result += hex[character >> 4U]; result += hex[character & 15U];
        } else result += static_cast<char>(character);
    }
    return result + '"';
}
/** @summary Serializes a finite float with roundtrip precision and a fixed locale. */
[[nodiscard]] inline std::string JsonFloat(float value) {
    if (!std::isfinite(value)) return "null";
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(9) << value;
    return stream.str();
}
/** @summary Converts a native filesystem UTF-8 representation to a narrow string. */
[[nodiscard]] inline std::string Utf8Text(std::u8string_view text) {
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}
}
