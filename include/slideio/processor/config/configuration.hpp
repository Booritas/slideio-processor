// Requirements: UR-EXE-2, UR-EXE-3, UR-EXE-6, UR-OBS-6
#pragma once

/**
 * @file configuration.hpp
 * @brief Configuration and ConfigurationBuilder: the immutable settings of a run.
 */

#include <slideio/processor/core/export.hpp>
#include <slideio/processor/core/log.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

/**
 * @brief An immutable value object holding every library setting.
 *
 * Every Workspace, Pipeline and Store captures a copy at construction and never
 * re-reads the process default, so a setting cannot change mid-run. Build one
 * with ConfigurationBuilder, or take defaults().
 *
 * A moved-from Configuration reads as defaults().
 * @par Thread-safety
 * Immutable. The process default is ThreadSafe.
 * @par Requirements
 * UR-EXE-2, UR-EXE-3, UR-EXE-6, UR-OBS-6
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS Configuration {
public:
    /**
     * @brief Builds a Configuration holding the documented defaults.
     * @note The worker count is read from the hardware when this runs.
     * @throws std::bad_alloc When memory is exhausted.
     */
    Configuration();
    /// Copies a configuration.
    /// @throws std::bad_alloc When memory is exhausted.
    Configuration(const Configuration&);
    /// Moves a configuration; the source then reads as defaults(). A failure to
    /// allocate the replacement state of the source is fatal and calls std::terminate.
    Configuration(Configuration&&) noexcept;
    /// Copy-assigns a configuration.
    /// @throws std::bad_alloc When memory is exhausted.
    Configuration& operator=(const Configuration&);
    /// Move-assigns a configuration; the source then reads as defaults(). A
    /// failure to allocate the replacement state of the source is fatal and calls
    /// std::terminate.
    Configuration& operator=(Configuration&&) noexcept;
    ~Configuration();

    /**
     * @brief Returns a configuration holding the documented defaults.
     * @return The defaults. workerCount is the hardware concurrency, or 1 when
     * that is unknown.
     * @note Declared noexcept although it allocates: failure to allocate is
     * fatal and calls std::terminate.
     */
    static Configuration defaults() noexcept;

    /**
     * @brief Returns the process-wide default, used when a Configuration is not supplied.
     * @return A reference that stays valid until the process exits and keeps
     * reading the values it was published with, even after setProcessDefault().
     * Equals defaults() until something is set.
     * @note Declared noexcept although the first call may allocate: failure to
     * allocate is fatal and calls std::terminate.
     */
    static const Configuration& processDefault() noexcept;
    /**
     * @brief Replaces the process-wide default.
     * @param configuration The new default; copied.
     * @details Intended at startup only. Objects already constructed are
     * unaffected. Every earlier default is kept alive until the process exits,
     * so references from processDefault() never dangle; each call therefore
     * costs one Configuration of memory.
     * @throws std::bad_alloc When memory is exhausted.
     */
    static void setProcessDefault(const Configuration& configuration);

    /// @brief Returns the memory ceiling in bytes (default 2 GiB).
    int64_t memoryCeilingBytes() const noexcept;
    /// @brief Returns the worker count (default hardware concurrency; 1 = single-threaded).
    int32_t workerCount() const noexcept;
    /// @brief Returns the tile width in pixels (default 1024).
    int32_t tileWidth() const noexcept;
    /// @brief Returns the tile height in pixels (default 1024).
    int32_t tileHeight() const noexcept;
    /// @brief Tells whether tile size is derived from the native tiling of the slide (default true).
    bool tileSizeFromSlide() const noexcept;
    /// @brief Returns the log file path; "" means stderr (default ""). Never null.
    const char* logFilePath() const noexcept;
    /// @brief Returns the log threshold (default Info).
    LogLevel logLevel() const noexcept;
    /// @brief Returns the event poll interval in milliseconds (default 150).
    int32_t eventPollIntervalMs() const noexcept;
    /// @brief Tells whether the local wake-up signal is enabled (default true).
    bool eventSignalEnabled() const noexcept;
    /// @brief Returns the object batch size (default 512).
    int64_t objectBatchSize() const noexcept;
    /// @brief Returns where a directly-constructed Pipeline spills seam
    /// fragments; "" means the platform temporary directory (default ""). Never
    /// null. A Workspace overrides it with a subdirectory of its own.
    const char* scratchDirectory() const noexcept;

    /**
     * @brief Compares two configurations.
     * @param other The other configuration.
     * @return True when every setting is equal; strings compare by content.
     */
    bool equals(const Configuration& other) const noexcept;

private:
    friend class ConfigurationBuilder;
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief Collects settings and validates them into a Configuration.
 *
 * Setters never validate; build() does, and names every offending setting.
 * A null string passed to a setter is recorded and reported by build().
 * A moved-from builder reads as a default-constructed one.
 * @par Thread-safety
 * ThreadConfined.
 * @par Requirements
 * UR-EXE-2, UR-EXE-3, UR-EXE-6, UR-OBS-6
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS ConfigurationBuilder {
public:
    /// Starts from the documented defaults.
    /// @throws std::bad_alloc When memory is exhausted.
    ConfigurationBuilder();
    /// Starts from an existing configuration.
    /// @throws std::bad_alloc When memory is exhausted.
    explicit ConfigurationBuilder(const Configuration& from);
    /// Copies a builder.
    /// @throws std::bad_alloc When memory is exhausted.
    ConfigurationBuilder(const ConfigurationBuilder&);
    /// Moves a builder; the source then reads as a default-constructed one. A
    /// failure to allocate the replacement state of the source is fatal and calls
    /// std::terminate.
    ConfigurationBuilder(ConfigurationBuilder&&) noexcept;
    /// Copy-assigns a builder.
    /// @throws std::bad_alloc When memory is exhausted.
    ConfigurationBuilder& operator=(const ConfigurationBuilder&);
    /// Move-assigns a builder; the source then reads as a default-constructed one.
    /// A failure to allocate the replacement state of the source is fatal and
    /// calls std::terminate.
    ConfigurationBuilder& operator=(ConfigurationBuilder&&) noexcept;
    ~ConfigurationBuilder();

    /// @brief Sets the memory ceiling in bytes; build() requires > 0.
    ConfigurationBuilder& memoryCeilingBytes(int64_t bytes);
    /// @brief Sets the worker count (1 = single-threaded); build() requires >= 1.
    ConfigurationBuilder& workerCount(int32_t count);
    /// @brief Sets the tile size; build() requires both >= 1.
    ConfigurationBuilder& tileSize(int32_t width, int32_t height);
    /// @brief Sets whether tile size follows the native tiling of the slide.
    ConfigurationBuilder& tileSizeFromSlide(bool fromSlide);
    /// @brief Sets the log file path ("" = stderr); the text is copied. build() rejects null.
    /// @throws std::bad_alloc When memory is exhausted.
    ConfigurationBuilder& logFilePath(const char* path);
    /// @brief Sets the log threshold; build() requires Trace..Off.
    ConfigurationBuilder& logLevel(LogLevel level);
    /// @brief Sets the event poll interval in milliseconds; build() requires >= 1.
    ConfigurationBuilder& eventPollIntervalMs(int32_t milliseconds);
    /// @brief Sets whether the local wake-up signal is enabled.
    ConfigurationBuilder& eventSignalEnabled(bool enabled);
    /// @brief Sets the object batch size; build() requires >= 1.
    ConfigurationBuilder& objectBatchSize(int64_t size);
    /// @brief Sets the scratch directory ("" = platform temporary directory); the
    /// text is copied. build() rejects null.
    /// @throws std::bad_alloc When memory is exhausted.
    ConfigurationBuilder& scratchDirectory(const char* path);

    /**
     * @brief Validates the whole configuration and builds it.
     * @return An independent Configuration; later builder changes do not affect it.
     * @throws InvalidArgumentError Naming every offending setting, in declaration
     * order, not just the first.
     * @throws std::bad_alloc When memory is exhausted.
     */
    Configuration build() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
