// Requirements: UR-API-5, UR-EXE-5, UR-RES-12
#pragma once

#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/export.hpp>

#include <cstdint>
#include <exception>
#include <memory>

namespace slideio::processor {

/**
 * @brief Category of a ProcessorError.
 * @par Requirements
 * UR-API-5, UR-EXE-5
 */
enum class ErrorCode : int32_t {
    InvalidArgument, ///< A caller-supplied argument is invalid.
    UnsupportedDataType, ///< A pixel data type is not supported.
    ChannelArity, ///< A channel count does not match what an operation needs.
    StoreFormat, ///< A store has an unreadable or incompatible format.
    Stage, ///< A pipeline stage failed.
    Tile, ///< Processing of a tile failed.
    Cancelled, ///< The operation was cancelled.
    Io, ///< An input/output operation failed.
    Internal ///< A broken invariant or an unclassified third-party failure.
};

/**
 * @brief Optional diagnostic context for a ProcessorError.
 *
 * Plain data, zero-initialised by default. Every string is UTF-8, may be null
 * (read as empty), and is copied by the error constructor, so the pointers
 * need only stay valid for the duration of that call.
 * @par Thread-safety
 * A value type: distinct instances may be used concurrently.
 * @par Requirements
 * UR-API-5, UR-EXE-5
 */
struct SLIDEIO_PROCESSOR_DATA_EXPORTS ErrorContext {
    const char* stageName = nullptr; ///< Name of the failing stage, or null.
    const char* operationName = nullptr; ///< Name of the failing operation, or null.
    const char* cause = nullptr; ///< Text of the underlying failure, or null.
    bool hasTile = false; ///< True when tileX and tileY are meaningful.
    int64_t tileX = 0; ///< Tile column index.
    int64_t tileY = 0; ///< Tile row index.
};

/**
 * @brief Base class of every error that crosses the public API boundary.
 *
 * Exception stance (C-6): std::bad_alloc is the only exception that is not a
 * ProcessorError and may still cross the boundary; it is always documented
 * where it can occur and never wrapped. Every other standard or third-party
 * exception
 * (std::system_error, std::filesystem::filesystem_error,
 * std::ios_base::failure, std::length_error and so on) is translated: to
 * IoError for an input/output failure, otherwise to InternalError, with the
 * original what() as cause(). A lock failure inside a noexcept function is
 * fatal (std::terminate) and is documented on that function.
 *
 * what() gives the message, then "; stage: S", "; operation: O",
 * "; tile: (X, Y)" and "; cause: C", each only when present, in that order.
 * The text is composed once at construction. Copies share it, and copying
 * never throws.
 * @par Thread-safety
 * Immutable after construction: safe to read from any thread.
 * @par Requirements
 * UR-API-5, UR-EXE-5, UR-RES-12
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS ProcessorError : public std::exception {
public:
    /**
     * @brief Copies an error. Shares the immutable message text.
     * @param other The error to copy.
     * @throws Nothing.
     */
    ProcessorError(const ProcessorError& other) noexcept;
    /**
     * @brief Assigns an error. Shares the immutable message text.
     * @param other The error to copy.
     * @return This object.
     * @throws Nothing.
     */
    ProcessorError& operator=(const ProcessorError& other) noexcept;
    /** @brief Destroys the error. */
    ~ProcessorError() override;

    /**
     * @brief Returns the composed message.
     * @return A null-terminated UTF-8 string that lives as long as this error
     * or any copy of it. Never null.
     * @throws Nothing.
     */
    const char* what() const noexcept override;
    /**
     * @brief Returns the error category.
     * @return The code.
     * @throws Nothing.
     */
    ErrorCode code() const noexcept;
    /**
     * @brief Returns the failing stage name.
     * @return A UTF-8 string, empty when unset. Never null.
     * @throws Nothing.
     */
    const char* stageName() const noexcept;
    /**
     * @brief Returns the failing operation name.
     * @return A UTF-8 string, empty when unset. Never null.
     * @throws Nothing.
     */
    const char* operationName() const noexcept;
    /**
     * @brief Returns the text of the underlying failure.
     * @return A UTF-8 string, empty when unset. Never null.
     * @throws Nothing.
     */
    const char* cause() const noexcept;
    /**
     * @brief Tells whether the error names a tile.
     * @return True when tileX() and tileY() are meaningful.
     * @throws Nothing.
     */
    bool hasTile() const noexcept;
    /**
     * @brief Returns the tile column index.
     * @return The index, 0 when hasTile() is false.
     * @throws Nothing.
     */
    int64_t tileX() const noexcept;
    /**
     * @brief Returns the tile row index.
     * @return The index, 0 when hasTile() is false.
     * @throws Nothing.
     */
    int64_t tileY() const noexcept;

protected:
    /**
     * @brief Builds an error. Used by subclasses.
     * @param code Error category.
     * @param message UTF-8 message; null reads as empty, because an exception
     * constructor must not throw for that reason.
     * @param context Diagnostic context; its strings are copied.
     * @throws std::bad_alloc When the text cannot be allocated.
     */
    ProcessorError(ErrorCode code, const char* message, const ErrorContext& context);

private:
    struct Impl;
    std::shared_ptr<const Impl> impl_;
};

/**
 * @brief A caller-supplied argument is invalid.
 * @par Thread-safety
 * Immutable after construction.
 * @par Requirements
 * UR-API-5
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS InvalidArgumentError : public ProcessorError {
public:
    /**
     * @brief Builds the error.
     * @param message UTF-8 message; null reads as empty.
     * @param context Diagnostic context; its strings are copied.
     * @throws std::bad_alloc When the text cannot be allocated.
     */
    explicit InvalidArgumentError(const char* message, const ErrorContext& context = ErrorContext());
};

/**
 * @brief A pixel data type is not supported by an operation.
 * @par Thread-safety
 * Immutable after construction.
 * @par Requirements
 * UR-API-5, UR-IMG-11
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS UnsupportedDataTypeError : public ProcessorError {
public:
    /**
     * @brief Builds the error.
     * @param message UTF-8 message; null reads as empty.
     * @param offendingType The type that was rejected.
     * @param supportedTypes The types the operation does accept.
     * @param context Diagnostic context; its strings are copied.
     * @throws std::bad_alloc When the text cannot be allocated.
     */
    UnsupportedDataTypeError(const char* message, DataType offendingType, DataTypeSet supportedTypes,
                             const ErrorContext& context = ErrorContext());
    /**
     * @brief Returns the rejected type.
     * @return The type.
     * @throws Nothing.
     */
    DataType offendingType() const noexcept;
    /**
     * @brief Returns the accepted types.
     * @return The set.
     * @throws Nothing.
     */
    DataTypeSet supportedTypes() const noexcept;

private:
    DataType offendingType_;
    DataTypeSet supportedTypes_;
};

/**
 * @brief A channel count does not match what an operation needs.
 * @par Thread-safety
 * Immutable after construction.
 * @par Requirements
 * UR-API-5
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS ChannelArityError : public ProcessorError {
public:
    /**
     * @brief Builds the error.
     * @param message UTF-8 message; null reads as empty.
     * @param offendingChannelCount The channel count that was rejected.
     * @param context Diagnostic context; its strings are copied.
     * @throws std::bad_alloc When the text cannot be allocated.
     */
    ChannelArityError(const char* message, int32_t offendingChannelCount,
                      const ErrorContext& context = ErrorContext());
    /**
     * @brief Returns the rejected channel count.
     * @return The channel count.
     * @throws Nothing.
     */
    int32_t offendingChannelCount() const noexcept;

private:
    int32_t offendingChannelCount_;
};

/**
 * @brief A store has an unreadable or incompatible format.
 * @par Thread-safety
 * Immutable after construction.
 * @par Requirements
 * UR-API-5, UR-RES-12
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS StoreFormatError : public ProcessorError {
public:
    /**
     * @brief Builds the error.
     * @param message UTF-8 message; null reads as empty.
     * @param storeVersion Format version found in the store.
     * @param libraryVersion Format version this library supports.
     * @param context Diagnostic context; its strings are copied.
     * @throws std::bad_alloc When the text cannot be allocated.
     */
    StoreFormatError(const char* message, int32_t storeVersion, int32_t libraryVersion,
                     const ErrorContext& context = ErrorContext());
    /**
     * @brief Returns the format version found in the store.
     * @return The version.
     * @throws Nothing.
     */
    int32_t storeVersion() const noexcept;
    /**
     * @brief Returns the format version this library supports.
     * @return The version.
     * @throws Nothing.
     */
    int32_t libraryVersion() const noexcept;

private:
    int32_t storeVersion_;
    int32_t libraryVersion_;
};

/**
 * @brief A pipeline stage failed.
 * @par Thread-safety
 * Immutable after construction.
 * @par Requirements
 * UR-API-5, UR-EXE-5
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS StageError : public ProcessorError {
public:
    /**
     * @brief Builds the error.
     * @param message UTF-8 message; null reads as empty.
     * @param context Diagnostic context; its strings are copied.
     * @throws std::bad_alloc When the text cannot be allocated.
     */
    explicit StageError(const char* message, const ErrorContext& context = ErrorContext());
};

/**
 * @brief Processing of a tile failed.
 * @par Thread-safety
 * Immutable after construction.
 * @par Requirements
 * UR-API-5, UR-EXE-5
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS TileError : public ProcessorError {
public:
    /**
     * @brief Builds the error.
     * @param message UTF-8 message; null reads as empty.
     * @param context Diagnostic context; its strings are copied.
     * @throws std::bad_alloc When the text cannot be allocated.
     */
    explicit TileError(const char* message, const ErrorContext& context = ErrorContext());
};

/**
 * @brief The operation was cancelled.
 * @par Thread-safety
 * Immutable after construction.
 * @par Requirements
 * UR-API-5, UR-EXE-5
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS CancelledError : public ProcessorError {
public:
    /**
     * @brief Builds the error.
     * @param message UTF-8 message; null reads as empty.
     * @param context Diagnostic context; its strings are copied.
     * @throws std::bad_alloc When the text cannot be allocated.
     */
    explicit CancelledError(const char* message, const ErrorContext& context = ErrorContext());
};

/**
 * @brief An input/output operation failed.
 * @par Thread-safety
 * Immutable after construction.
 * @par Requirements
 * UR-API-5
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS IoError : public ProcessorError {
public:
    /**
     * @brief Builds the error.
     * @param message UTF-8 message; null reads as empty.
     * @param context Diagnostic context; its strings are copied.
     * @throws std::bad_alloc When the text cannot be allocated.
     */
    explicit IoError(const char* message, const ErrorContext& context = ErrorContext());
};

/**
 * @brief A broken invariant or a third-party failure with no better category.
 * @par Thread-safety
 * Immutable after construction.
 * @par Requirements
 * UR-API-5
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS InternalError : public ProcessorError {
public:
    /**
     * @brief Builds the error.
     * @param message UTF-8 message; null reads as empty.
     * @param context Diagnostic context; its strings are copied.
     * @throws std::bad_alloc When the text cannot be allocated.
     */
    explicit InternalError(const char* message, const ErrorContext& context = ErrorContext());
};

} // namespace slideio::processor
