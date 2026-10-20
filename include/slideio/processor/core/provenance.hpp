// Requirements: UR-RES-11
#pragma once

/**
 * @file provenance.hpp
 * @brief Provenance: the slide-level provenance record written with every export.
 */

#include <slideio/processor/core/export.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

/**
 * @brief The slide-level provenance record: which slide and scene, which
 * library version, and when.
 *
 * A default-constructed Provenance reports the running library's version and
 * the time it was constructed. A record read back from a store, possibly
 * written by another version at another time, is rebuilt with restored(),
 * which reproduces the stored values; there is no setter for either.
 *
 * Per-stage configuration is recorded by the workspace, not here. After move
 * construction or move assignment the source is in a default state: it reads
 * as slide id "", scene 0, and the running library's version, and a setter
 * re-initialises it.
 * @par Thread-safety
 * ThreadCompatible: const members may run concurrently; any mutation needs
 * exclusive access.
 * @par Requirements
 * UR-RES-11
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS Provenance {
public:
    /**
     * @brief Builds a record with slide id "", scene index 0, the library
     * version, and the current time as creation time.
     * @throws std::bad_alloc When memory is exhausted.
     */
    Provenance();
    /**
     * @brief Copies a record, including its creation time.
     * @param other The record to copy.
     * @throws std::bad_alloc When memory is exhausted.
     */
    Provenance(const Provenance& other);
    /**
     * @brief Moves a record. The source is left in the default state described
     * in the class comment.
     * @param other The record to move from.
     * @throws Nothing.
     */
    Provenance(Provenance&& other) noexcept;
    /**
     * @brief Copy-assigns a record.
     * @param other The record to copy.
     * @return This object.
     * @throws std::bad_alloc When memory is exhausted; this object is unchanged.
     */
    Provenance& operator=(const Provenance& other);
    /**
     * @brief Move-assigns a record. The source is left in the default state
     * described in the class comment; never swaps.
     * @param other The record to move from.
     * @return This object.
     * @throws Nothing.
     */
    Provenance& operator=(Provenance&& other) noexcept;
    /** @brief Destroys the record. */
    ~Provenance();

    /**
     * @brief Returns the slide identifier.
     * @return A null-terminated UTF-8 string, never null, valid until the next
     * mutation or destruction.
     * @throws Nothing.
     */
    const char* slideId() const noexcept;
    /**
     * @brief Returns the scene index.
     * @return The index, at least 0.
     * @throws Nothing.
     */
    int32_t sceneIndex() const noexcept;
    /**
     * @brief Returns the library version recorded in this provenance.
     *
     * The running library's version for a default-constructed record; the
     * stored version for one built by restored().
     * @return A null-terminated string, never null, valid while this object lives.
     * @throws Nothing.
     */
    const char* libraryVersion() const noexcept;
    /**
     * @brief Returns the creation time.
     * @return The time this object was constructed, in milliseconds since the
     * Unix epoch (system clock); copies keep the source's value, and a
     * moved-from record reads 0.
     * @throws Nothing.
     */
    int64_t createdAtUnixMs() const noexcept;

    /**
     * @brief Sets the slide identifier.
     * @param slideId UTF-8 identifier, copied.
     * @throws InvalidArgumentError When slideId is null; nothing changes.
     * @throws std::bad_alloc When memory is exhausted; nothing changes.
     */
    void setSlideId(const char* slideId);
    /**
     * @brief Sets the scene index.
     * @param sceneIndex The index, at least 0.
     * @throws InvalidArgumentError When sceneIndex is negative; nothing changes.
     */
    void setSceneIndex(int32_t sceneIndex);
    /**
     * @brief Writes the record as compact JSON,
     * {"slideId":...,"sceneIndex":...,"libraryVersion":...,"createdAtUnixMs":...},
     * in that key order.
     * @param buffer Receives the null-terminated UTF-8 text; null queries the
     * length.
     * @param capacity The buffer size in bytes including the terminator. When
     * too small, the text is truncated and still terminated; when 0, nothing
     * is written.
     * @return The required length excluding the terminator.
     * @throws InternalError When serialisation fails.
     * @throws std::bad_alloc When memory is exhausted.
     */
    int32_t toJson(char* buffer, int32_t capacity) const;

    /**
     * @brief Rebuilds a provenance record from stored values.
     *
     * For code reading a store. Not a setter: it only reproduces what was
     * persisted, and the result reads back exactly these values. toJson() of
     * the result carries the restored version and time.
     * @param slideId UTF-8 slide identifier, copied; null reads as "".
     * @param sceneIndex The scene index, stored as given.
     * @param libraryVersion UTF-8 library version, copied; null reads as "".
     * @param createdAtUnixMs The creation time, in milliseconds since the Unix epoch.
     * @return The restored record.
     * @throws std::bad_alloc When memory is exhausted.
     */
    static Provenance restored(const char* slideId, int32_t sceneIndex, const char* libraryVersion,
                               int64_t createdAtUnixMs);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
