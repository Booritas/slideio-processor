// Requirements: UR-RES-8, UR-RES-11, UR-RES-15, UR-RES-18
#pragma once

#include <slideio/processor/core/export.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/core/provenance.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

/**
 * @brief Streams objects into a GeoJSON FeatureCollection file.
 *
 * Call begin() once, writeFeature() once per object, then end(). Each feature
 * is written as it arrives, one per line, so exporting ten million objects
 * stays within the memory ceiling. A feature is built whole before any of it
 * reaches the file, so a rejected feature leaves no trace.
 *
 * A polygon with exactly one outer ring becomes a GeoJSON Polygon; any other
 * becomes a MultiPolygon in which each hole belongs to the first outer ring,
 * in ring order, containing the hole's first vertex (a vertex on the boundary
 * counts as inside), or to the first outer ring when no outer ring contains
 * it. Rings are written closed with integer coordinates.
 *
 * The writer is move-only. The destructor never throws and does not complete
 * the document: a writer destroyed before end() leaves a truncated file. After
 * a write failure end() throws IoError, so a failed export never looks
 * successful. A moved-from writer rejects every call with InvalidArgumentError.
 * @par Thread-safety
 * ThreadConfined.
 * @par Requirements
 * UR-RES-8, UR-RES-11, UR-RES-15, UR-RES-18
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS GeoJsonWriter {
public:
    /**
     * @brief Creates or truncates the file.
     * @param path UTF-8 file path, opened through std::filesystem::u8path.
     * @throws InvalidArgumentError When path is null.
     * @throws IoError When the file cannot be opened; the message names the path.
     * @throws std::bad_alloc When memory is exhausted.
     */
    explicit GeoJsonWriter(const char* path);
    /**
     * @brief Moves a writer; the source becomes unusable.
     * @param other The writer to move from.
     * @throws Nothing.
     */
    GeoJsonWriter(GeoJsonWriter&& other) noexcept;
    /**
     * @brief Move-assigns a writer, closing this one's file without completing it.
     * @param other The writer to move from.
     * @return This object.
     * @throws Nothing.
     */
    GeoJsonWriter& operator=(GeoJsonWriter&& other) noexcept;
    GeoJsonWriter(const GeoJsonWriter&) = delete;
    GeoJsonWriter& operator=(const GeoJsonWriter&) = delete;
    /** @brief Closes the file without completing the document. Never throws. */
    ~GeoJsonWriter();

    /**
     * @brief Writes the collection header and the provenance record.
     * @param provenance The provenance, written as the "provenance" member.
     * @throws InvalidArgumentError When called twice or after end().
     * @throws IoError When writing fails.
     * @throws InternalError When serialising the provenance fails.
     */
    void begin(const Provenance& provenance);
    /**
     * @brief Writes one object as a Feature.
     * @param record The object; its geometry must not be empty and every
     * measurement value must be finite.
     * @param layerName UTF-8 layer name; not null.
     * @param labelNames The object's effective label names, in the set's fixed
     * order; `labelCount` entries, none null. May be null when labelCount is 0.
     * @param labelIsWorkspaceLocal Parallel flags, `labelCount` entries. May be
     * null when labelCount is 0.
     * @param labelCount Number of labels, at least 0. The first label, when
     * there is one, is also written as the "classification".
     * @throws InvalidArgumentError Before begin(), after end(), when layerName
     * is null, when labelCount is negative, when labelCount > 0 and an array or
     * a name is null, when the geometry is empty, or when a measurement is not
     * finite. Nothing is written in these cases.
     * @throws IoError When writing fails.
     * @throws std::bad_alloc When memory is exhausted.
     */
    void writeFeature(const ObjectRecord& record,
                      const char* layerName,
                      const char* const* labelNames,
                      const bool* labelIsWorkspaceLocal,
                      int32_t labelCount);
    /**
     * @brief Closes the collection, flushes and checks the stream.
     * @throws InvalidArgumentError Before begin() or when already ended.
     * @throws IoError When the stream failed at any point since opening.
     */
    void end();
    /**
     * @brief Returns the number of features written so far.
     * @return The count.
     * @throws Nothing.
     */
    int64_t featuresWritten() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
