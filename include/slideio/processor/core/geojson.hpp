// Requirements: UR-RES-8, UR-RES-11, UR-RES-13, UR-RES-15, UR-RES-18
#pragma once

/**
 * @file geojson.hpp
 * @brief Streaming GeoJSON reader (GeoJsonReader) and writer (GeoJsonWriter).
 */

#include <slideio/processor/core/export.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/core/provenance.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

/**
 * @brief Streams Features out of a GeoJSON file, one at a time.
 *
 * Accepts a FeatureCollection (its "features" array; other members are
 * skipped), a top-level array of Features, or a single Feature. The file is
 * pull-parsed: next() reads only as far as the end of the next feature, so
 * memory is bounded by one feature whatever the file size.
 *
 * A Polygon's first ring is an outer ring and the rest are holes; a
 * MultiPolygon contributes each member polygon the same way. Coordinates are
 * parsed independently of the C locale and rounded to the nearest integer,
 * halves away from zero. A leading UTF-8 byte order mark is skipped; byte
 * offsets in messages still count it. A feature whose geometry is null, missing,
 * empty, or of any other type is skipped with a LogLevel::Warning message
 * naming its index and type.
 *
 * Properties are the feature's scalar "properties" members other than
 * "classification" and "layer", in document order, followed by the scalar
 * members of "properties.measurements", named "measurements.<key>". Values are
 * text: strings as their content, numbers exactly as written in the file,
 * booleans as "true" or "false". Nulls, arrays and other nested objects are
 * skipped.
 *
 * Feature indices in messages are 0-based positions in the feature list,
 * counting skipped features. A geometry error (a ring with fewer than 3
 * distinct points, a non-finite or out-of-range coordinate, or malformed
 * coordinate nesting) throws from next() but leaves the reader usable: the
 * next call continues with the following feature. A parse or read error is
 * final: every later next() throws again.
 *
 * Pointers returned by classification(), layerName(), propertyName() and
 * propertyValue(), and the reference returned by geometry(), stay valid until
 * the next call to next(), or until the reader is moved from or destroyed.
 * Before the first successful next(), and after next() returns false or
 * throws, the accessors report an empty feature.
 *
 * The reader is move-only. A moved-from reader reports an empty feature, and
 * its next() throws InvalidArgumentError.
 * @par Thread-safety
 * ThreadConfined.
 * @par Requirements
 * UR-RES-8, UR-RES-13, UR-RES-15
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS GeoJsonReader {
public:
    /**
     * @brief Opens the file. Nothing is parsed until next().
     * @param path UTF-8 file path, opened through std::filesystem::u8path.
     * @throws InvalidArgumentError When path is null.
     * @throws IoError When the file cannot be opened; the message names the path.
     * @throws std::bad_alloc When memory is exhausted.
     */
    explicit GeoJsonReader(const char* path);
    /**
     * @brief Moves a reader. The source is left moved-from: it reports an empty
     * feature and its next() throws InvalidArgumentError.
     * @param other The reader to move from.
     * @throws Nothing.
     */
    GeoJsonReader(GeoJsonReader&& other) noexcept;
    /**
     * @brief Move-assigns a reader, closing this one's file. The source is left
     * moved-from, as after move construction; never swaps.
     * @param other The reader to move from.
     * @return This object.
     * @throws Nothing.
     */
    GeoJsonReader& operator=(GeoJsonReader&& other) noexcept;
    /** @brief Deleted: the reader is move-only. */
    GeoJsonReader(const GeoJsonReader&) = delete;
    /**
     * @brief Deleted: the reader is move-only.
     * @return Not applicable.
     */
    GeoJsonReader& operator=(const GeoJsonReader&) = delete;
    /** @brief Closes the file. Never throws. */
    ~GeoJsonReader();

    /**
     * @brief Advances to the next feature with supported geometry.
     *
     * Invalidates every pointer and reference returned by the accessors.
     * @return True when a feature was read; false at the end of the document.
     * @throws InvalidArgumentError When the JSON is malformed or the top-level
     * value is not an object or an array (the message names the path, the byte
     * offset and the parser's error text); when a ring has fewer than 3
     * distinct points, a coordinate is not finite or does not fit int64_t, or
     * the coordinates are malformed (the message names the feature's index);
     * after an earlier parse or read error; or on a moved-from reader.
     * @throws IoError When reading the file fails.
     * @throws InternalError When the parser fails for any other reason.
     * @throws std::bad_alloc When memory is exhausted.
     */
    bool next();
    /**
     * @brief Returns the current feature's geometry.
     * @return The geometry; empty before the first successful next().
     * @throws Nothing.
     */
    const Polygon& geometry() const noexcept;
    /**
     * @brief Returns the current feature's classification.
     * @return properties.classification.name when classification is an object,
     * the string itself when it is a string, otherwise "". Never null.
     * @throws Nothing.
     */
    const char* classification() const noexcept;
    /**
     * @brief Returns the current feature's layer name.
     * @return properties.layer when it is a string, otherwise "". Never null.
     * @throws Nothing.
     */
    const char* layerName() const noexcept;
    /**
     * @brief Returns the number of properties of the current feature.
     * @return The count, at least 0.
     * @throws Nothing.
     */
    int32_t propertyCount() const noexcept;
    /**
     * @brief Returns a property's name.
     * @param index 0 <= index < propertyCount().
     * @return The UTF-8 name. Never null.
     * @throws InvalidArgumentError When index is out of range.
     */
    const char* propertyName(int32_t index) const;
    /**
     * @brief Returns a property's value as text.
     * @param index 0 <= index < propertyCount().
     * @return The UTF-8 value. Never null.
     * @throws InvalidArgumentError When index is out of range.
     */
    const char* propertyValue(int32_t index) const;
    /**
     * @brief Returns the number of features next() has returned so far.
     * @return The count; skipped and failed features are not counted.
     * @throws Nothing.
     */
    int64_t featuresRead() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

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
     * @brief Moves a writer. The source is left moved-from: it rejects every
     * call with InvalidArgumentError.
     * @param other The writer to move from.
     * @throws Nothing.
     */
    GeoJsonWriter(GeoJsonWriter&& other) noexcept;
    /**
     * @brief Move-assigns a writer, closing this one's file without completing
     * it. The source is left moved-from, as after move construction; never
     * swaps.
     * @param other The writer to move from.
     * @return This object.
     * @throws Nothing.
     */
    GeoJsonWriter& operator=(GeoJsonWriter&& other) noexcept;
    /** @brief Deleted: the writer is move-only. */
    GeoJsonWriter(const GeoJsonWriter&) = delete;
    /**
     * @brief Deleted: the writer is move-only.
     * @return Not applicable.
     */
    GeoJsonWriter& operator=(const GeoJsonWriter&) = delete;
    /** @brief Closes the file without completing the document. Never throws. */
    ~GeoJsonWriter();

    /**
     * @brief Writes the collection header and the provenance record.
     * @param provenance The provenance, written as the "provenance" member.
     * @throws InvalidArgumentError When called twice or after end().
     * @throws IoError When writing fails.
     * @throws InternalError When serialising the provenance fails.
     * @throws std::bad_alloc When memory is exhausted.
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
     * @throws std::bad_alloc When memory is exhausted.
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
