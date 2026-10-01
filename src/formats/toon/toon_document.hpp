#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace sc13::formats::toon {

/** Stores one JSON-shaped value decoded from the supported TOON 4.1 subset. */
class Value final {
public:
    using Object = std::map<std::string, Value, std::less<>>;
    using Array = std::vector<Value>;
    using Storage = std::variant<std::nullptr_t, bool, double, std::string, Object, Array>;

    /** Constructs the TOON null value. */
    Value() noexcept = default;

    /** Constructs a boolean primitive. */
    explicit Value(bool value) noexcept : storage_(value) {}

    /** Constructs a finite numeric primitive. */
    explicit Value(double value) noexcept : storage_(value) {}

    /** Constructs a string primitive. */
    explicit Value(std::string value) : storage_(std::move(value)) {}

    /** Constructs an object value. */
    explicit Value(Object value) : storage_(std::move(value)) {}

    /** Constructs an array value. */
    explicit Value(Array value) : storage_(std::move(value)) {}

    /** Returns the object storage when this value is an object. */
    [[nodiscard]] const Object* AsObject() const noexcept;

    /** Returns mutable object storage when this value is an object. */
    [[nodiscard]] Object* AsObject() noexcept;

    /** Returns the array storage when this value is an array. */
    [[nodiscard]] const Array* AsArray() const noexcept;

    /** Returns the string storage when this value is a string. */
    [[nodiscard]] const std::string* AsString() const noexcept;

    /** Returns the numeric storage when this value is numeric. */
    [[nodiscard]] const double* AsNumber() const noexcept;

    /** Returns the boolean storage when this value is boolean. */
    [[nodiscard]] const bool* AsBoolean() const noexcept;

    /** Finds one direct object member without allocating. */
    [[nodiscard]] const Value* Find(std::string_view key) const noexcept;

private:
    Storage storage_{nullptr};
};

/** Reports one bounded source location and diagnostic for malformed TOON. */
struct ParseError final {
    std::size_t line{};
    std::size_t column{};
    std::string message;
};

/** Owns one decoded root object. */
class Document final {
public:
    /** Returns the decoded root object. */
    [[nodiscard]] const Value::Object& root() const noexcept { return root_; }

private:
    friend bool Parse(std::string_view, Document&, ParseError&) noexcept;
    Value::Object root_;
};

/** Decodes a strict, documented TOON 4.1 subset into a JSON-shaped document. */
[[nodiscard]] bool Parse(
    std::string_view text,
    Document& document,
    ParseError& error) noexcept;

/** Loads and decodes a bounded UTF-8 TOON file. */
[[nodiscard]] bool LoadFile(
    const std::filesystem::path& path,
    Document& document,
    ParseError& error) noexcept;

}  // namespace sc13::formats::toon
