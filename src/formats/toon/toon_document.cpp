#include "formats/toon/toon_document.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <system_error>

namespace sc13::formats::toon {
namespace {

constexpr std::size_t kIndentSize = 2U;
constexpr std::size_t kMaximumDepth = 32U;
constexpr std::size_t kMaximumLines = 10'000U;
constexpr std::size_t kMaximumFileSize = 1024U * 1024U;

/** Stores one normalized non-empty logical input line. */
struct Line final {
    std::size_t number{};
    std::size_t depth{};
    std::string_view text;
};

/** Describes a parsed array declaration such as patches[1]. */
struct ArrayHeader final {
    std::string key;
    std::size_t count{};
    std::vector<std::string> fields;
};

/** Trims spaces from both ends of one token. */
[[nodiscard]] std::string_view Trim(std::string_view value) noexcept {
    while (!value.empty() && value.front() == ' ') {
        value.remove_prefix(1U);
    }
    while (!value.empty() && value.back() == ' ') {
        value.remove_suffix(1U);
    }
    return value;
}

/** Reports a parser failure at one logical line. */
[[nodiscard]] bool Fail(
    ParseError& error,
    const Line& line,
    std::size_t column,
    std::string message) {
    error.line = line.number;
    error.column = column;
    error.message = std::move(message);
    return false;
}

/** Splits one comma-delimited row while preserving quoted commas and escapes. */
[[nodiscard]] bool SplitRow(
    std::string_view text,
    const Line& line,
    std::vector<std::string_view>& tokens,
    ParseError& error) {
    tokens.clear();
    bool quoted = false;
    bool escaped = false;
    std::size_t tokenBegin = 0U;
    for (std::size_t index = 0U; index < text.size(); ++index) {
        const char character = text[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (quoted && character == '\\') {
            escaped = true;
            continue;
        }
        if (character == '"') {
            quoted = !quoted;
            continue;
        }
        if (!quoted && character == ',') {
            tokens.push_back(Trim(text.substr(tokenBegin, index - tokenBegin)));
            tokenBegin = index + 1U;
        }
    }
    if (quoted || escaped) {
        return Fail(error, line, text.size() + 1U, "Unterminated quoted value");
    }
    tokens.push_back(Trim(text.substr(tokenBegin)));
    return true;
}

/** Decodes the quoted-string escapes used by the M3 TOON schemas. */
[[nodiscard]] bool DecodeQuoted(
    std::string_view token,
    const Line& line,
    std::string& value,
    ParseError& error) {
    if (token.size() < 2U || token.front() != '"' || token.back() != '"') {
        return Fail(error, line, 1U, "Quoted string is missing its closing quote");
    }
    value.clear();
    value.reserve(token.size() - 2U);
    for (std::size_t index = 1U; index + 1U < token.size(); ++index) {
        const char character = token[index];
        if (character != '\\') {
            value.push_back(character);
            continue;
        }
        if (++index + 1U >= token.size()) {
            return Fail(error, line, index + 1U, "Incomplete string escape");
        }
        switch (token[index]) {
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            default:
                return Fail(error, line, index + 1U, "Unsupported string escape");
        }
    }
    return true;
}

/** Decodes one primitive token according to TOON's JSON-shaped scalar rules. */
[[nodiscard]] bool ParsePrimitive(
    std::string_view token,
    const Line& line,
    Value& value,
    ParseError& error) {
    token = Trim(token);
    if (token.empty()) {
        value = Value(std::string{});
        return true;
    }
    if (token.front() == '"') {
        std::string decoded;
        if (!DecodeQuoted(token, line, decoded, error)) {
            return false;
        }
        value = Value(std::move(decoded));
        return true;
    }
    if (token == "null") {
        value = Value();
        return true;
    }
    if (token == "true") {
        value = Value(true);
        return true;
    }
    if (token == "false") {
        value = Value(false);
        return true;
    }

    double number = 0.0;
    const char* const begin = token.data();
    const char* const end = begin + token.size();
    const auto result = std::from_chars(begin, end, number, std::chars_format::general);
    if (result.ec == std::errc{} && result.ptr == end && std::isfinite(number)) {
        value = Value(number);
        return true;
    }
    value = Value(std::string(token));
    return true;
}

/** Parses a decimal array length without accepting signs or overflow. */
[[nodiscard]] bool ParseCount(
    std::string_view text,
    const Line& line,
    std::size_t& count,
    ParseError& error) {
    if (text.empty()) {
        return Fail(error, line, 1U, "Array length is empty");
    }
    std::uint64_t parsed = 0U;
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        parsed > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return Fail(error, line, 1U, "Array length is not a valid decimal integer");
    }
    count = static_cast<std::size_t>(parsed);
    return true;
}

/** Parses a supported comma-delimited TOON array header. */
[[nodiscard]] bool ParseArrayHeader(
    std::string_view keyText,
    const Line& line,
    std::optional<ArrayHeader>& header,
    ParseError& error) {
    header.reset();
    const std::size_t open = keyText.find('[');
    if (open == std::string_view::npos) {
        return true;
    }
    const std::size_t close = keyText.find(']', open + 1U);
    if (close == std::string_view::npos) {
        return Fail(error, line, open + 1U, "Array header is missing ']'");
    }
    ArrayHeader parsed;
    const std::string_view key = Trim(keyText.substr(0U, open));
    if (key.empty()) {
        return Fail(error, line, 1U, "Array field key is empty");
    }
    parsed.key.assign(key);
    if (!ParseCount(keyText.substr(open + 1U, close - open - 1U), line, parsed.count, error)) {
        return false;
    }
    std::string_view suffix = Trim(keyText.substr(close + 1U));
    if (!suffix.empty()) {
        if (suffix.size() < 2U || suffix.front() != '{' || suffix.back() != '}') {
            return Fail(error, line, close + 2U, "Unsupported array header suffix");
        }
        suffix.remove_prefix(1U);
        suffix.remove_suffix(1U);
        std::vector<std::string_view> fields;
        if (!SplitRow(suffix, line, fields, error)) {
            return false;
        }
        if (fields.empty()) {
            return Fail(error, line, close + 2U, "Tabular field list is empty");
        }
        for (const std::string_view field : fields) {
            if (field.empty() || field.find_first_of("{}[]:") != std::string_view::npos) {
                return Fail(
                    error, line, close + 2U,
                    "Only flat field names are supported in M3 tabular arrays");
            }
            parsed.fields.emplace_back(field);
        }
    }
    header = std::move(parsed);
    return true;
}

/** Finds the member separator outside quoted text. */
[[nodiscard]] std::size_t FindMemberColon(std::string_view text) noexcept {
    bool quoted = false;
    bool escaped = false;
    for (std::size_t index = 0U; index < text.size(); ++index) {
        const char character = text[index];
        if (escaped) {
            escaped = false;
        } else if (quoted && character == '\\') {
            escaped = true;
        } else if (character == '"') {
            quoted = !quoted;
        } else if (!quoted && character == ':') {
            return index;
        }
    }
    return std::string_view::npos;
}

/** Implements the bounded recursive-descent parser over normalized lines. */
class Parser final {
public:
    /** Constructs a parser for one stable normalized line view. */
    Parser(std::span<const Line> lines, ParseError& error) noexcept
        : lines_(lines), error_(error) {}

    /** Parses the required root object and consumes every line. */
    [[nodiscard]] bool ParseRoot(Value::Object& root) {
        if (lines_.empty()) {
            root.clear();
            return true;
        }
        if (lines_.front().depth != 0U) {
            return Fail(error_, lines_.front(), 1U, "Root object must start at depth zero");
        }
        if (!ParseObject(0U, root)) {
            return false;
        }
        if (index_ != lines_.size()) {
            return Fail(error_, lines_[index_], 1U, "Unexpected trailing scope");
        }
        return true;
    }

private:
    /** Inserts one unique object member. */
    [[nodiscard]] bool Insert(
        Value::Object& object,
        std::string key,
        Value value,
        const Line& line) {
        const auto [iterator, inserted] = object.emplace(std::move(key), std::move(value));
        static_cast<void>(iterator);
        if (!inserted) {
            return Fail(error_, line, 1U, "Duplicate object key");
        }
        return true;
    }

    /** Parses sibling members at one exact depth. */
    [[nodiscard]] bool ParseObject(std::size_t depth, Value::Object& object) {
        if (depth > kMaximumDepth) {
            return Fail(error_, lines_[index_], 1U, "Maximum nesting depth exceeded");
        }
        while (index_ < lines_.size() && lines_[index_].depth == depth) {
            const Line line = lines_[index_++];
            if (line.text.starts_with("-")) {
                return Fail(error_, line, 1U, "List item is not valid in an object scope");
            }
            if (!ParseMember(line.text, depth, line, object)) {
                return false;
            }
        }
        return true;
    }

    /** Parses one object member and any scope it opens. */
    [[nodiscard]] bool ParseMember(
        std::string_view text,
        std::size_t depth,
        const Line& line,
        Value::Object& object) {
        const std::size_t colon = FindMemberColon(text);
        if (colon == std::string_view::npos) {
            return Fail(error_, line, 1U, "Object member is missing ':'");
        }
        const std::string_view keyText = Trim(text.substr(0U, colon));
        const std::string_view payload = Trim(text.substr(colon + 1U));
        std::optional<ArrayHeader> arrayHeader;
        if (!ParseArrayHeader(keyText, line, arrayHeader, error_)) {
            return false;
        }
        if (arrayHeader.has_value()) {
            Value::Array array;
            if (!ParseArray(*arrayHeader, payload, depth, line, array)) {
                return false;
            }
            return Insert(object, arrayHeader->key, Value(std::move(array)), line);
        }

        if (keyText.empty() || keyText.find_first_of("[]{}") != std::string_view::npos) {
            return Fail(error_, line, 1U, "Object key is empty or malformed");
        }
        if (!payload.empty()) {
            Value primitive;
            if (!ParsePrimitive(payload, line, primitive, error_)) {
                return false;
            }
            return Insert(object, std::string(keyText), std::move(primitive), line);
        }

        Value::Object nested;
        if (index_ < lines_.size() && lines_[index_].depth > depth) {
            if (lines_[index_].depth != depth + 1U) {
                return Fail(error_, lines_[index_], 1U, "Object indentation jumps a depth");
            }
            if (!ParseObject(depth + 1U, nested)) {
                return false;
            }
        }
        return Insert(object, std::string(keyText), Value(std::move(nested)), line);
    }

    /** Parses an inline, tabular, or list-form array with exact count validation. */
    [[nodiscard]] bool ParseArray(
        const ArrayHeader& header,
        std::string_view payload,
        std::size_t depth,
        const Line& line,
        Value::Array& array) {
        if (!payload.empty()) {
            if (!header.fields.empty()) {
                return Fail(error_, line, 1U, "Tabular array cannot have inline values");
            }
            std::vector<std::string_view> tokens;
            if (!SplitRow(payload, line, tokens, error_)) {
                return false;
            }
            if (tokens.size() != header.count) {
                return Fail(error_, line, 1U, "Inline array length does not match its header");
            }
            for (const std::string_view token : tokens) {
                Value primitive;
                if (!ParsePrimitive(token, line, primitive, error_)) {
                    return false;
                }
                array.push_back(std::move(primitive));
            }
            return true;
        }
        if (header.count == 0U) {
            return true;
        }
        if (!header.fields.empty()) {
            return ParseTabularArray(header, depth, line, array);
        }
        return ParseListArray(header, depth, line, array);
    }

    /** Parses flat tabular object rows. */
    [[nodiscard]] bool ParseTabularArray(
        const ArrayHeader& header,
        std::size_t depth,
        const Line& headerLine,
        Value::Array& array) {
        for (std::size_t row = 0U; row < header.count; ++row) {
            if (index_ >= lines_.size() || lines_[index_].depth != depth + 1U) {
                return Fail(
                    error_, index_ < lines_.size() ? lines_[index_] : headerLine, 1U,
                    "Tabular array row count does not match its header");
            }
            const Line line = lines_[index_++];
            if (line.text.starts_with("-")) {
                return Fail(error_, line, 1U, "Tabular row must not start with '-'");
            }
            std::vector<std::string_view> tokens;
            if (!SplitRow(line.text, line, tokens, error_)) {
                return false;
            }
            if (tokens.size() != header.fields.size()) {
                return Fail(error_, line, 1U, "Tabular row width does not match its field list");
            }
            Value::Object object;
            for (std::size_t field = 0U; field < header.fields.size(); ++field) {
                Value primitive;
                if (!ParsePrimitive(tokens[field], line, primitive, error_)) {
                    return false;
                }
                object.emplace(header.fields[field], std::move(primitive));
            }
            array.emplace_back(std::move(object));
        }
        if (index_ < lines_.size() && lines_[index_].depth > depth) {
            return Fail(error_, lines_[index_], 1U, "Unexpected extra tabular row or depth");
        }
        return true;
    }

    /** Parses list-form primitives and objects. */
    [[nodiscard]] bool ParseListArray(
        const ArrayHeader& header,
        std::size_t depth,
        const Line& headerLine,
        Value::Array& array) {
        for (std::size_t item = 0U; item < header.count; ++item) {
            if (index_ >= lines_.size() || lines_[index_].depth != depth + 1U) {
                return Fail(
                    error_, index_ < lines_.size() ? lines_[index_] : headerLine, 1U,
                    "List item count does not match its header");
            }
            const Line line = lines_[index_++];
            if (!line.text.starts_with("- ")) {
                return Fail(error_, line, 1U, "List item must start with '- '");
            }
            const std::string_view itemText = Trim(line.text.substr(2U));
            if (FindMemberColon(itemText) == std::string_view::npos) {
                Value primitive;
                if (!ParsePrimitive(itemText, line, primitive, error_)) {
                    return false;
                }
                array.push_back(std::move(primitive));
                continue;
            }

            Value::Object object;
            const std::size_t memberDepth = depth + 2U;
            if (!ParseMember(itemText, memberDepth, line, object)) {
                return false;
            }
            while (index_ < lines_.size() && lines_[index_].depth == memberDepth) {
                const Line memberLine = lines_[index_++];
                if (!ParseMember(memberLine.text, memberDepth, memberLine, object)) {
                    return false;
                }
            }
            array.emplace_back(std::move(object));
        }
        if (index_ < lines_.size() && lines_[index_].depth > depth) {
            return Fail(error_, lines_[index_], 1U, "Unexpected extra list item or depth");
        }
        return true;
    }

    std::span<const Line> lines_;
    ParseError& error_;
    std::size_t index_{};
};

/** Normalizes source lines and validates indentation before recursive parsing. */
[[nodiscard]] bool Tokenize(
    std::string_view text,
    std::vector<Line>& lines,
    ParseError& error) {
    lines.clear();
    std::size_t lineNumber = 1U;
    std::size_t cursor = 0U;
    while (cursor <= text.size()) {
        const std::size_t newline = text.find('\n', cursor);
        const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
        std::string_view sourceLine = text.substr(cursor, end - cursor);
        if (!sourceLine.empty() && sourceLine.back() == '\r') {
            sourceLine.remove_suffix(1U);
        }
        if (sourceLine.find('\r') != std::string_view::npos) {
            error = ParseError{lineNumber, 1U, "Unexpected carriage return"};
            return false;
        }
        while (!sourceLine.empty() && sourceLine.back() == ' ') {
            sourceLine.remove_suffix(1U);
        }
        if (!sourceLine.empty()) {
            std::size_t spaces = 0U;
            while (spaces < sourceLine.size() && sourceLine[spaces] == ' ') {
                ++spaces;
            }
            if (spaces < sourceLine.size() && sourceLine[spaces] == '\t') {
                error = ParseError{lineNumber, spaces + 1U, "Tabs are not valid indentation"};
                return false;
            }
            if ((spaces % kIndentSize) != 0U) {
                error = ParseError{lineNumber, spaces + 1U, "Indentation must use two spaces"};
                return false;
            }
            const std::size_t depth = spaces / kIndentSize;
            if (depth > kMaximumDepth) {
                error = ParseError{lineNumber, spaces + 1U, "Maximum nesting depth exceeded"};
                return false;
            }
            lines.push_back(Line{lineNumber, depth, sourceLine.substr(spaces)});
            if (lines.size() > kMaximumLines) {
                error = ParseError{lineNumber, 1U, "TOON document has too many lines"};
                return false;
            }
        }
        if (newline == std::string_view::npos) {
            break;
        }
        cursor = newline + 1U;
        ++lineNumber;
    }
    return true;
}

}  // namespace

const Value::Object* Value::AsObject() const noexcept {
    return std::get_if<Object>(&storage_);
}

Value::Object* Value::AsObject() noexcept {
    return std::get_if<Object>(&storage_);
}

const Value::Array* Value::AsArray() const noexcept {
    return std::get_if<Array>(&storage_);
}

const std::string* Value::AsString() const noexcept {
    return std::get_if<std::string>(&storage_);
}

const double* Value::AsNumber() const noexcept {
    return std::get_if<double>(&storage_);
}

const bool* Value::AsBoolean() const noexcept {
    return std::get_if<bool>(&storage_);
}

const Value* Value::Find(std::string_view key) const noexcept {
    const Object* const object = AsObject();
    if (object == nullptr) {
        return nullptr;
    }
    const auto iterator = object->find(key);
    return iterator == object->end() ? nullptr : &iterator->second;
}

bool Parse(
    std::string_view text,
    Document& document,
    ParseError& error) noexcept {
    try {
        document.root_.clear();
        error = {};
        if (text.size() > kMaximumFileSize) {
            error.message = "TOON document exceeds the 1 MiB limit";
            return false;
        }
        if (text.starts_with("\xEF\xBB\xBF")) {
            text.remove_prefix(3U);
        }
        std::vector<Line> lines;
        if (!Tokenize(text, lines, error)) {
            return false;
        }
        Parser parser(lines, error);
        Value::Object root;
        if (!parser.ParseRoot(root)) {
            return false;
        }
        document.root_ = std::move(root);
        return true;
    } catch (...) {
        document.root_.clear();
        error = ParseError{0U, 0U, "TOON parser failed due to an allocation exception"};
        return false;
    }
}

bool LoadFile(
    const std::filesystem::path& path,
    Document& document,
    ParseError& error) noexcept {
    try {
        std::error_code sizeError;
        const std::uintmax_t size = std::filesystem::file_size(path, sizeError);
        if (sizeError) {
            error = ParseError{0U, 0U, "Could not query TOON file size"};
            return false;
        }
        if (size > kMaximumFileSize) {
            error = ParseError{0U, 0U, "TOON file exceeds the 1 MiB limit"};
            return false;
        }
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            error = ParseError{0U, 0U, "Could not open TOON file"};
            return false;
        }
        std::string text(static_cast<std::size_t>(size), '\0');
        stream.read(text.data(), static_cast<std::streamsize>(text.size()));
        if (stream.gcount() != static_cast<std::streamsize>(text.size())) {
            error = ParseError{0U, 0U, "Could not read the complete TOON file"};
            return false;
        }
        return Parse(text, document, error);
    } catch (...) {
        document = {};
        error = ParseError{0U, 0U, "TOON file load failed due to an allocation exception"};
        return false;
    }
}

}  // namespace sc13::formats::toon
