// Requirements: UR-API-2, UR-STG-4
#pragma once

/**
 * @file export_spec.hpp
 * @brief ExportKind and ExportSpec: one output file of a slide run, named by kind.
 */

#include <slideio/processor/pipeline/export.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

/** @brief What an export writes. */
enum class ExportKind : int32_t {
    Csv,        ///< The objects of a layer as a CSV table.
    GeoJson,    ///< The objects of a layer as a GeoJSON feature collection.
    LabelMask,  ///< A layer's objects rasterised as a label mask, an OME-TIFF.
    RasterTiff  ///< A raster written by a stage, as a TIFF.
};

/**
 * @brief One export of a pipeline file: a kind, a plain output file name, and the layer
 * or raster it reads.
 *
 * Built through a named constructor per kind. The file name is a plain name inside the
 * export directory the run is given: it is refused when empty, `.` or `..`; when it
 * contains `/`, `\`, `:` or a control character; when it ends in `.` or a space (Windows
 * would drop them) or in `.partial` in any case (the writers' temporary name); and when it
 * is a Windows device name (CON, PRN, AUX, NUL, COM1 to COM9, LPT1 to LPT9, in any case,
 * with or without an extension). A CSV export may list measurement names; none means all
 * of them.
 * Messages name the export kind and the member, never the value.
 * @par Thread-safety
 * ThreadConfined to build; const members are safe to call concurrently.
 * @par Requirements
 * UR-API-2, UR-STG-4
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS ExportSpec {
public:
    /**
     * @brief A CSV export of a layer's objects, with every measurement until some are added.
     * @param file Plain output file name.
     * @param layer Name of the layer to export.
     * @return The export.
     * @throws InvalidArgumentError When a name is null or empty, or the file name is not a plain name.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-API-2
     */
    static ExportSpec csv(const char* file, const char* layer);
    /**
     * @brief A GeoJSON export of a layer's objects.
     * @param file Plain output file name.
     * @param layer Name of the layer to export.
     * @return The export.
     * @throws InvalidArgumentError When a name is null or empty, or the file name is not a plain name.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-API-2
     */
    static ExportSpec geoJson(const char* file, const char* layer);
    /**
     * @brief A label-mask export of a layer's objects.
     * @param file Plain output file name.
     * @param layer Name of the layer to export.
     * @return The export.
     * @throws InvalidArgumentError When a name is null or empty, or the file name is not a plain name.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-API-2
     */
    static ExportSpec labelMask(const char* file, const char* layer);
    /**
     * @brief A TIFF export of a raster.
     * @param file Plain output file name.
     * @param raster Name of the raster to export.
     * @return The export.
     * @throws InvalidArgumentError When a name is null or empty, or the file name is not a plain name.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-API-2
     */
    static ExportSpec rasterTiff(const char* file, const char* raster);

    /**
     * @brief Copies an export; the copy shares nothing with the original.
     * @param other The export to copy.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Reads @p other; ThreadCompatible.
     * @par Requirements
     * UR-API-2
     */
    ExportSpec(const ExportSpec& other);
    /**
     * @brief Moves an export; the moved-from export may only be assigned to or destroyed.
     * @param other The export to move from.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-API-2
     */
    ExportSpec(ExportSpec&& other) noexcept;
    /**
     * @brief Replaces this export with a copy of another.
     * @param other The export to copy.
     * @return This export.
     * @throws std::bad_alloc When allocation fails; this export is then unchanged.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-API-2
     */
    ExportSpec& operator=(const ExportSpec& other);
    /**
     * @brief Replaces this export by moving another.
     * @param other The export to move from.
     * @return This export.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-API-2
     */
    ExportSpec& operator=(ExportSpec&& other) noexcept;
    ~ExportSpec();

    /**
     * @brief Adds a measurement column to a CSV export.
     * @param name Standard measurement name, non-null and non-empty.
     * @return This export, for chaining.
     * @throws InvalidArgumentError When this is not a CSV export or the name is null or empty.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-API-2
     */
    ExportSpec& measurement(const char* name);

    /**
     * @brief Returns the kind.
     * @return The kind.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently on a const export.
     * @par Requirements
     * UR-API-2
     */
    ExportKind kind() const noexcept;
    /**
     * @brief Returns the output file name.
     * @return A UTF-8 name, valid until the export is modified or destroyed.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently on a const export.
     * @par Requirements
     * UR-API-2
     */
    const char* file() const noexcept;
    /**
     * @brief Returns the layer an object export reads.
     * @return The layer name, or null for a raster export.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently on a const export.
     * @par Requirements
     * UR-API-2
     */
    const char* layer() const noexcept;
    /**
     * @brief Returns the raster a raster export reads.
     * @return The raster name, or null for an object export.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently on a const export.
     * @par Requirements
     * UR-API-2
     */
    const char* raster() const noexcept;
    /**
     * @brief Returns the number of listed measurements; 0 means every measurement.
     * @return The count; always 0 for kinds other than CSV.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently on a const export.
     * @par Requirements
     * UR-API-2
     */
    int32_t measurementCount() const noexcept;
    /**
     * @brief Returns a listed measurement name.
     * @param index Position, from 0 to measurementCount() - 1.
     * @return The name; valid until the export is modified or destroyed.
     * @throws InvalidArgumentError When index is out of range.
     * @par Thread-safety
     * Safe to call concurrently on a const export.
     * @par Requirements
     * UR-API-2
     */
    const char* measurementAt(int32_t index) const;

    /**
     * @brief Compares two exports by value: kind, file name, layer or raster, and the
     * measurement list in order.
     * @param other The export to compare with.
     * @return True when equal.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently on const exports.
     * @par Requirements
     * UR-API-2
     */
    bool equals(const ExportSpec& other) const noexcept;

private:
    class Impl;
    explicit ExportSpec(std::unique_ptr<Impl> impl) noexcept;
    static ExportSpec make(ExportKind kind, const char* file, const char* target);
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
