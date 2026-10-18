// Requirements: UR-IMG-10, UR-IMG-11, UR-IMG-12, UR-BLD-4
#pragma once

/**
 * @file backend_capabilities.hpp
 * @brief BackendCapabilities: what the resolved computation backend supports.
 */

#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/export.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

/**
 * @brief Reports the computation backend the library was built against, and
 * the data types it computes natively.
 *
 * Generated at build time from the backend actually resolved. Under OpenCV 4
 * eight of the eleven types are computable, with UInt32, Int64 and UInt64 in
 * storageOnlyTypes(); under OpenCV 5 the three move across and
 * storageOnlyTypes() empties. Nothing here names the OpenCV 4 type set as a
 * fixed limitation (UR-BLD-4).
 *
 * The library carries all eleven types losslessly through reading, channel
 * selection, conversion and storage regardless of backend; only an algorithm
 * may decline one (UR-IMG-10, UR-IMG-11). This is the input to the generated
 * support matrix of UR-IMG-12.
 * @par Thread-safety
 * Immutable.
 * @par Requirements
 * UR-IMG-11, UR-IMG-12, UR-IMG-10, UR-BLD-4
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS BackendCapabilities {
public:
    /**
     * @brief Returns the one instance.
     * @return A reference valid for the life of the process; always the same object.
     * @throws Nothing.
     */
    static const BackendCapabilities& instance() noexcept;

    /**
     * @brief Returns the backend's name.
     * @return For example "OpenCV". Never null; static storage.
     * @throws Nothing.
     */
    const char* backendName() const noexcept;
    /**
     * @brief Returns the backend's version.
     * @return For example "4.9.0". Never null; static storage.
     * @throws Nothing.
     */
    const char* backendVersion() const noexcept;
    /**
     * @brief Returns the backend's major version.
     * @return The major version, for example 4.
     * @throws Nothing.
     */
    int32_t backendMajorVersion() const noexcept;

    /**
     * @brief Returns the types the backend computes natively.
     * @return The set; never contains Unknown.
     * @throws Nothing.
     */
    DataTypeSet supportedTypes() const noexcept;
    /**
     * @brief Tells whether the backend computes a type natively.
     * @param type The type.
     * @return True when it is in supportedTypes(); false for Unknown and
     * out-of-range values.
     * @throws Nothing.
     */
    bool supports(DataType type) const noexcept;

    /// Types carried losslessly by the library but not computed natively by
    /// the backend. Reading, selection, conversion and storage remain lossless
    /// for these (UR-IMG-10); only algorithms may decline them (UR-IMG-11).
    /**
     * @brief Returns the types carried losslessly but not computed natively.
     * @return All eleven types minus supportedTypes().
     * @throws Nothing.
     */
    DataTypeSet storageOnlyTypes() const noexcept;

    BackendCapabilities(const BackendCapabilities&) = delete;
    BackendCapabilities& operator=(const BackendCapabilities&) = delete;

private:
    BackendCapabilities() noexcept;
    ~BackendCapabilities();

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
