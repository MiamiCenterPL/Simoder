#include "memory/signature.hpp"

#include <charconv>

namespace sc13::memory {

std::optional<Signature> Signature::Parse(std::string_view text, std::string& error) noexcept {
    try {
        std::vector<SignatureByte> bytes;
        std::size_t offset = 0;
        while (offset < text.size()) {
            while (offset < text.size() && text[offset] == ' ') {
                ++offset;
            }
            if (offset == text.size()) {
                break;
            }

            const std::size_t end = text.find(' ', offset);
            const std::string_view token = text.substr(offset, end - offset);
            if (token == "?" || token == "??") {
                bytes.emplace_back(std::nullopt);
            } else {
                if (token.size() != 2) {
                    error = "Every concrete signature token must contain exactly two hex digits";
                    return std::nullopt;
                }
                unsigned int value = 0;
                const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value, 16);
                if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || value > 0xFFU) {
                    error = "Signature contains an invalid hexadecimal byte";
                    return std::nullopt;
                }
                bytes.emplace_back(static_cast<std::uint8_t>(value));
            }
            offset = end == std::string_view::npos ? text.size() : end + 1;
        }

        if (bytes.empty()) {
            error = "Signature must not be empty";
            return std::nullopt;
        }
        return Signature(std::move(bytes));
    } catch (...) {
        error = "Signature parsing failed due to an allocation exception";
        return std::nullopt;
    }
}

std::vector<std::size_t> FindAll(std::span<const std::byte> haystack, const Signature& signature) {
    std::vector<std::size_t> matches;
    const auto pattern = signature.bytes();
    if (pattern.size() > haystack.size()) {
        return matches;
    }

    for (std::size_t start = 0; start <= haystack.size() - pattern.size(); ++start) {
        bool matchesHere = true;
        for (std::size_t index = 0; index < pattern.size(); ++index) {
            if (pattern[index].has_value() &&
                static_cast<std::uint8_t>(haystack[start + index]) != pattern[index].value()) {
                matchesHere = false;
                break;
            }
        }
        if (matchesHere) {
            matches.push_back(start);
        }
    }
    return matches;
}

UniqueSignatureResult FindUnique(
    std::span<const std::byte> haystack, const Signature& signature) {
    const std::vector<std::size_t> matches = FindAll(haystack, signature);
    if (matches.empty()) {
        return UniqueSignatureResult{UniqueSignatureStatus::NotFound, 0U, 0U};
    }
    if (matches.size() != 1U) {
        return UniqueSignatureResult{UniqueSignatureStatus::Ambiguous, 0U, matches.size()};
    }
    return UniqueSignatureResult{UniqueSignatureStatus::Matched, matches.front(), 1U};
}

}  // namespace sc13::memory
