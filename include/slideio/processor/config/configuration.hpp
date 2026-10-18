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
    /**
     * @brief Copies a configuration.
     * @param other The configuration to copy.
     * @throws std::bad_alloc When memory is exhausted.
     */
    Configuration(const Configuration& other);
    /**
     * @brief Moves a configuration; the source then reads as defaults().
     * @param other The configuration to move from.
     * @throws Nothing. A failure to allocate the replacement state of the source
     * is fatal and calls std::terminate.
     */
    Configuration(Configuration&& other) noexcept;
    /**
     * @brief Copy-assigns a configuration.
     * @param other The configuration to copy.
     * @return This object.
     * @throws std::bad_alloc When memory is exhausted; this object is unchanged.
     */
    Configuration& operator=(const Configuration& other);
    /**
     * @brief Move-assigns a configuration; the source then reads as defaults().
     * Never swaps.
     * @param other The configuration to move from.
     * @return This object.
     * @throws Nothing. A failure to allocate the replacement state of the source
     * is fatal and calls std::terminate.
     */
    Configuration& operator=(Configuration&& other) noexcept;
    /// Destroys the configuration.
    ~Configuration();

    /**
     * @brief Returns a configuration holding the documented defaults.
     * @return The defaults. workerCount is the hardware concurrency, or 1 when
     * that is unknown.
     * @throws Nothing. Declared noexcept although it allocates: failure to
     * allocate is fatal and calls std::terminate.
     */
    static Configuration defaults() noexcept;

    /**
     * @brief Returns the process-wide default, used when a Configuration is not supplied.
     * @return A reference that stays valid until the process exits and keeps
     * reading the values it was published with, even after setProcessDefault().
     * Equals defaults() until something is set.
     * @throws Nothing. Declared noexcept although the first call allocates and
     * every call takes a mutex: failure to allocate or to lock is fatal and calls
     * std::terminate.
     * @par Thread-safety
     * ThreadSafe.
     */
    static const Configuration& processDefault() noexcept;
    /**
     * @brief Replaces the process-wide default.
     * @param configuration The new default; copied.
     * @details Intended at startup only. Objects already constructed are
     * unaffected. Every earlier default is kept alive until the process exits,
     * so references from processDefault() never dangle; each call therefore
     * costs one Configuration of memory. Logging is not reconfigured; see
     * configureLogging().
     * @throws InternalError When the library's lock cannot be acquired or another
     * unexpected standard exception other than std::bad_alloc is raised; its
     * message is the cause.
     * @throws std::bad_alloc When memory is exhausted; the default is unchanged.
     * @par Thread-safety
     * ThreadSafe.
     */
    static void setProcessDefault(const Configuration& configuration);

    /**
     * @brief Returns the memory ceiling in bytes (default 2 GiB).
     * @return The ceiling.
     * @throws Nothing.
     */
    int64_t memoryCeilingBytes() const noexcept;
    /**
     * @brief Returns the worker count (default hardware concurrency; 1 = single-threaded).
     * @return The worker count.
     * @throws Nothing.
     */
    int32_t workerCount() const noexcept;
    /**
     * @brief Returns the tile width in pixels (default 1024).
     * @return The tile width.
     * @throws Nothing.
     */
    int32_t tileWidth() const noexcept;
    /**
     * @brief Returns the tile height in pixels (default 1024).
     * @return The tile height.
     * @throws Nothing.
     */
    int32_t tileHeight() const noexcept;
    /**
     * @brief Tells whether tile size is derived from the native tiling of the slide (default true).
     * @return True when the native tiling is used.
     * @throws Nothing.
     */
    bool tileSizeFromSlide() const noexcept;
    /**
     * @brief Returns the log file path; "" means stderr (default "").
     * @return The path. Never null; valid while this object lives and is not
     * modified or moved from.
     * @throws Nothing.
     * @note Takes effect only through configureLogging(). Until then core
     * discards messages below its own threshold (null sink, Warning), and
     * setProcessDefault() does not reconfigure logging.
     */
    const char* logFilePath() const noexcept;
    /**
     * @brief Returns the log threshold (default Info).
     * @return The threshold.
     * @throws Nothing.
     * @note Takes effect only through configureLogging(). Until then core
     * discards messages below its own threshold (null sink, Warning), and
     * setProcessDefault() does not reconfigure logging.
     */
    LogLevel logLevel() const noexcept;
    /**
     * @brief Returns the event poll interval in milliseconds (default 150).
     * @return The interval.
     * @throws Nothing.
     */
    int32_t eventPollIntervalMs() const noexcept;
    /**
     * @brief Tells whether the local wake-up signal is enabled (default true).
     * @return True when enabled.
     * @throws Nothing.
     */
    bool eventSignalEnabled() const noexcept;
    /**
     * @brief Returns the object batch size (default 512).
     * @return The batch size.
     * @throws Nothing.
     */
    int64_t objectBatchSize() const noexcept;
    /**
     * @brief Returns where a directly-constructed Pipeline spills seam
     * fragments; "" means the platform temporary directory (default "").
     * @return The directory. Never null; valid while this object lives and is not
     * modified or moved from. A Workspace overrides it with a subdirectory of its own.
     * @throws Nothing.
     */
    const char* scratchDirectory() const noexcept;

    /**
     * @brief Compares two configurations.
     * @param other The other configuration.
     * @return True when every setting is equal; strings compare by content.
     * @throws Nothing.
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
    /**
     * @brief Starts from the documented defaults.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ConfigurationBuilder();
    /**
     * @brief Starts from an existing configuration.
     * @param from The configuration whose settings are copied.
     * @throws std::bad_alloc When memory is exhausted.
     */
    explicit ConfigurationBuilder(const Configuration& from);
    /**
     * @brief Copies a builder.
     * @param other The builder to copy.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ConfigurationBuilder(const ConfigurationBuilder& other);
    /**
     * @brief Moves a builder; the source then reads as a default-constructed one.
     * @param other The builder to move from.
     * @throws Nothing. A failure to allocate the replacement state of the source
     * is fatal and calls std::terminate.
     */
    ConfigurationBuilder(ConfigurationBuilder&& other) noexcept;
    /**
     * @brief Copy-assigns a builder.
     * @param other The builder to copy.
     * @return This object.
     * @throws std::bad_alloc When memory is exhausted; this object is unchanged.
     */
    ConfigurationBuilder& operator=(const ConfigurationBuilder& other);
    /**
     * @brief Move-assigns a builder; the source then reads as a default-constructed
     * one. Never swaps.
     * @param other The builder to move from.
     * @return This object.
     * @throws Nothing. A failure to allocate the replacement state of the source
     * is fatal and calls std::terminate.
     */
    ConfigurationBuilder& operator=(ConfigurationBuilder&& other) noexcept;
    /// Destroys the builder.
    ~ConfigurationBuilder();

    /**
     * @brief Sets the memory ceiling in bytes; build() requires > 0.
     * @param bytes The ceiling.
     * @return This builder.
     * @throws Nothing.
     */
    ConfigurationBuilder& memoryCeilingBytes(int64_t bytes);
    /**
     * @brief Sets the worker count (1 = single-threaded); build() requires >= 1.
     * @param count The worker count.
     * @return This builder.
     * @throws Nothing.
     */
    ConfigurationBuilder& workerCount(int32_t count);
    /**
     * @brief Sets the tile size; build() requires both >= 1.
     * @param width The tile width in pixels.
     * @param height The tile height in pixels.
     * @return This builder.
     * @throws Nothing.
     */
    ConfigurationBuilder& tileSize(int32_t width, int32_t height);
    /**
     * @brief Sets whether tile size follows the native tiling of the slide.
     * @param fromSlide True to use the native tiling.
     * @return This builder.
     * @throws Nothing.
     */
    ConfigurationBuilder& tileSizeFromSlide(bool fromSlide);
    /**
     * @brief Sets the log file path ("" = stderr); the text is copied. build() rejects null.
     * @param path UTF-8 path; may be null, which build() reports.
     * @return This builder.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ConfigurationBuilder& logFilePath(const char* path);
    /**
     * @brief Sets the log threshold; build() requires Trace..Off.
     * @param level The threshold.
     * @return This builder.
     * @throws Nothing.
     */
    ConfigurationBuilder& logLevel(LogLevel level);
    /**
     * @brief Sets the event poll interval in milliseconds; build() requires >= 1.
     * @param milliseconds The interval.
     * @return This builder.
     * @throws Nothing.
     */
    ConfigurationBuilder& eventPollIntervalMs(int32_t milliseconds);
    /**
     * @brief Sets whether the local wake-up signal is enabled.
     * @param enabled True to enable it.
     * @return This builder.
     * @throws Nothing.
     */
    ConfigurationBuilder& eventSignalEnabled(bool enabled);
    /**
     * @brief Sets the object batch size; build() requires >= 1.
     * @param size The batch size.
     * @return This builder.
     * @throws Nothing.
     */
    ConfigurationBuilder& objectBatchSize(int64_t size);
    /**
     * @brief Sets the scratch directory ("" = platform temporary directory); the
     * text is copied. build() rejects null.
     * @param path UTF-8 path; may be null, which build() reports.
     * @return This builder.
     * @throws std::bad_alloc When memory is exhausted.
     */
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
