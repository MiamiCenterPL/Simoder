#include "core/sha256.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <vector>

namespace sc13::core {
namespace {

/** Owns a CNG algorithm provider handle. */
class AlgorithmHandle final {
public:
    /** Closes the provider when one was opened. */
    ~AlgorithmHandle() {
        if (value_ != nullptr) {
            BCryptCloseAlgorithmProvider(value_, 0);
        }
    }

    AlgorithmHandle(const AlgorithmHandle&) = delete;
    AlgorithmHandle& operator=(const AlgorithmHandle&) = delete;
    AlgorithmHandle() = default;

    /** Returns a writable pointer for BCryptOpenAlgorithmProvider. */
    [[nodiscard]] BCRYPT_ALG_HANDLE* put() noexcept { return &value_; }

    /** Returns the provider handle. */
    [[nodiscard]] BCRYPT_ALG_HANDLE get() const noexcept { return value_; }

private:
    BCRYPT_ALG_HANDLE value_{};
};

/** Owns a CNG hash handle. */
class HashHandle final {
public:
    /** Destroys the hash object when one was created. */
    ~HashHandle() {
        if (value_ != nullptr) {
            BCryptDestroyHash(value_);
        }
    }

    HashHandle(const HashHandle&) = delete;
    HashHandle& operator=(const HashHandle&) = delete;
    HashHandle() = default;

    /** Returns a writable pointer for BCryptCreateHash. */
    [[nodiscard]] BCRYPT_HASH_HANDLE* put() noexcept { return &value_; }

    /** Returns the hash handle. */
    [[nodiscard]] BCRYPT_HASH_HANDLE get() const noexcept { return value_; }

private:
    BCRYPT_HASH_HANDLE value_{};
};

/** Streams bytes into a newly created SHA-256 hash. */
template <typename Producer>
[[nodiscard]] bool ComputeHash(Producer&& producer, Sha256Digest& digest, std::string& error) noexcept {
    try {
        AlgorithmHandle algorithm;
        NTSTATUS status = BCryptOpenAlgorithmProvider(algorithm.put(), BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        if (status < 0) {
            error = "BCryptOpenAlgorithmProvider failed";
            return false;
        }

        DWORD objectLength = 0;
        DWORD resultLength = 0;
        status = BCryptGetProperty(
            algorithm.get(), BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength),
            sizeof(objectLength), &resultLength, 0);
        if (status < 0 || resultLength != sizeof(objectLength)) {
            error = "BCryptGetProperty(BCRYPT_OBJECT_LENGTH) failed";
            return false;
        }

        std::vector<UCHAR> object(objectLength);
        HashHandle hash;
        status = BCryptCreateHash(
            algorithm.get(), hash.put(), object.data(), static_cast<ULONG>(object.size()), nullptr, 0, 0);
        if (status < 0) {
            error = "BCryptCreateHash failed";
            return false;
        }

        if (!producer(hash.get(), error)) {
            return false;
        }

        status = BCryptFinishHash(
            hash.get(), reinterpret_cast<PUCHAR>(digest.data()), static_cast<ULONG>(digest.size()), 0);
        if (status < 0) {
            error = "BCryptFinishHash failed";
            return false;
        }
        return true;
    } catch (...) {
        error = "SHA-256 computation failed due to an allocation or I/O exception";
        return false;
    }
}

}  // namespace

bool Sha256(std::span<const std::byte> data, Sha256Digest& digest, std::string& error) noexcept {
    return ComputeHash(
        [data](BCRYPT_HASH_HANDLE hash, std::string& producerError) noexcept {
            constexpr std::size_t maximumChunk =
                static_cast<std::size_t>((std::numeric_limits<ULONG>::max)());
            std::size_t offset = 0;
            while (offset < data.size()) {
                const std::size_t size = (std::min)(maximumChunk, data.size() - offset);
                const NTSTATUS status = BCryptHashData(
                    hash, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(data.data() + offset)),
                    static_cast<ULONG>(size), 0);
                if (status < 0) {
                    producerError = "BCryptHashData failed";
                    return false;
                }
                offset += size;
            }
            return true;
        },
        digest, error);
}

bool Sha256File(const std::filesystem::path& path, Sha256Digest& digest, std::string& error) noexcept {
    return ComputeHash(
        [&path](BCRYPT_HASH_HANDLE hash, std::string& producerError) {
            std::ifstream stream(path, std::ios::binary);
            if (!stream) {
                producerError = "Could not open file for SHA-256";
                return false;
            }

            std::array<char, 64 * 1024> buffer{};
            while (stream) {
                stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const std::streamsize read = stream.gcount();
                if (read > 0) {
                    const NTSTATUS status = BCryptHashData(
                        hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(read), 0);
                    if (status < 0) {
                        producerError = "BCryptHashData failed while hashing file";
                        return false;
                    }
                }
            }
            if (!stream.eof()) {
                producerError = "I/O error while hashing file";
                return false;
            }
            return true;
        },
        digest, error);
}

std::string ToHex(const Sha256Digest& digest) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string result(digest.size() * 2, '0');
    for (std::size_t index = 0; index < digest.size(); ++index) {
        const auto value = static_cast<unsigned char>(digest[index]);
        result[index * 2] = digits[value >> 4U];
        result[index * 2 + 1] = digits[value & 0x0FU];
    }
    return result;
}

}  // namespace sc13::core
