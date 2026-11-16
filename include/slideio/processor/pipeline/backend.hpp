// Requirements: UR-BLD-3, UR-BLD-4, UR-IMG-11, UR-IMG-12
#pragma once

/**
 * @file backend.hpp
 * @brief Backend: the adapter to the computation backend the engine links.
 */

#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/pipeline/export.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

/**
 * @brief The computation backend the engine library is linked against, and
 * the mapping between DataType and the backend's sample depths.
 *
 * The single place a backend migration touches. No backend type appears in a
 * public header: backend matrices live only inside the engine, and BlockView
 * is what crosses the boundary. The supported set is derived from the backend
 * in use and always equals BackendCapabilities::supportedTypes(), so a newer
 * backend widens it without changing an API or a stored artifact.
 * @par Thread-safety
 * Immutable.
 * @par Requirements
 * UR-BLD-3, UR-BLD-4, UR-IMG-11, UR-IMG-12
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS Backend {
public:
    /**
     * @brief Returns the one instance.
     * @return A reference valid for the life of the process; always the same object.
     * @throws Nothing. Declared noexcept although the first call allocates:
     * failure to allocate is fatal and calls std::terminate.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-BLD-3
     */
    static const Backend& instance() noexcept;

    /**
     * @brief Returns the backend's name.
     * @return "OpenCV". Never null; static storage.
     * @throws Nothing.
     * @par Thread-safety
     * Immutable.
     * @par Requirements
     * UR-BLD-3
     */
    const char* name() const noexcept;
    /**
     * @brief Returns the version of the backend linked into the engine.
     * @return For example "4.14.0". Never null; valid for the life of the process.
     * @throws Nothing. Declared noexcept although the first call allocates:
     * failure to allocate is fatal and calls std::terminate.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-BLD-3, UR-BLD-4
     */
    const char* version() const noexcept;

    /**
     * @brief Tells whether the backend computes a type natively.
     * @param type The type.
     * @return True when it is in supportedTypes(); false for Unknown and
     * out-of-range values.
     * @throws Nothing.
     * @par Thread-safety
     * Immutable.
     * @par Requirements
     * UR-IMG-11, UR-BLD-4
     */
    bool supports(DataType type) const noexcept;
    /**
     * @brief Returns the types the backend computes natively.
     * @return Exactly the types toBackendDepth() maps; equal to
     * BackendCapabilities::supportedTypes(). Never contains Unknown.
     * @throws Nothing.
     * @par Thread-safety
     * Immutable.
     * @par Requirements
     * UR-IMG-11, UR-IMG-12, UR-BLD-4
     */
    DataTypeSet supportedTypes() const noexcept;

    /**
     * @brief Returns the backend's sample depth for a type.
     * @param type The type.
     * @return The backend's depth code for the type.
     * @throws UnsupportedDataTypeError When the backend does not compute the
     * type (including Unknown); the message names the type and the backend.
     * @throws std::bad_alloc When the error message cannot be allocated.
     * @par Thread-safety
     * Immutable.
     * @par Requirements
     * UR-IMG-11, UR-BLD-4
     */
    int32_t toBackendDepth(DataType type) const;
    /**
     * @brief Returns the type of a backend sample depth.
     * @param depth The backend's depth code.
     * @return The type, or DataType::Unknown for a depth the backend does not
     * map.
     * @throws Nothing.
     * @par Thread-safety
     * Immutable.
     * @par Requirements
     * UR-IMG-11, UR-BLD-4
     */
    DataType fromBackendDepth(int32_t depth) const noexcept;

    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

private:
    Backend() noexcept;
    ~Backend();

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
