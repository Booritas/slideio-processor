// Requirements: UR-STG-4, UR-RES-11
#pragma once

#include <slideio/processor/pipeline/operation_parameters.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace slideio::processor::detail {

/// Reads an operation's parameter bag strictly. Each getter returns the
/// fallback when the key is absent and marks the key as read; a present key of
/// the wrong type throws InvalidArgumentError. finish() throws for any key
/// never read, so a misspelt key is refused rather than replayed as a default.
/// Every message names the operation and the key, never a value. The bag must
/// outlive the reader and stay unchanged while it is used.
class BagReader {
public:
    BagReader(const char* operationName, const OperationParameters& parameters);

    double getDouble(const char* key, double fallback);
    int64_t getInt(const char* key, int64_t fallback);
    bool getBool(const char* key, bool fallback);
    /// The stored text, valid while the bag is unchanged.
    const char* getString(const char* key, const char* fallback);

    /// A string key holding one of @p count enumerator spellings; the result is
    /// the enumerator whose index the matching spelling has. Any other text
    /// throws InvalidArgumentError.
    template <typename E>
    E getEnum(const char* key, E fallback, const char* const* spellings, int32_t count) {
        const char* text = getString(key, nullptr);
        if (text == nullptr) {
            return fallback;
        }
        for (int32_t i = 0; i < count; ++i) {
            if (std::strcmp(text, spellings[i]) == 0) {
                return static_cast<E>(i);
            }
        }
        throwBadValue(key, "is not one of its accepted spellings");
    }

    /// Throws InvalidArgumentError naming the first key no getter has read.
    void finish() const;

    /// Throws InvalidArgumentError "<operation>: parameter '<key>' <problem>".
    [[noreturn]] void throwBadValue(const char* key, const char* problem) const;

private:
    /// Marks @p key read; returns whether the bag holds it.
    bool markRead(const char* key);

    std::string operation_;
    const OperationParameters& parameters_;
    std::vector<std::string> keys_;
    std::vector<bool> read_;
};

} // namespace slideio::processor::detail
