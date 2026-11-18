// Requirements: UR-SEG-5, UR-SEG-6, UR-IMG-2
#pragma once

/**
 * @file measure.hpp
 * @brief Per-object measurements: MeasurementKind, MeasurementSelection and
 * the scene calibration queries.
 *
 * The segmentation operations take a MeasurementSelection as a parameter and
 * measure each object as it is finished, whether it was found whole in one
 * tile or merged from fragments across tile seams. Geometric measurements are
 * in level-0 pixels; when the scene is calibrated each of them is also given
 * in micrometres, under the same name followed by `.um` (`area.um2` for the
 * area).
 */

#include <slideio/processor/pipeline/export.hpp>

#include <cstdint>
#include <memory>

namespace slideio {
class Scene;
} // namespace slideio

namespace slideio::processor {

/**
 * @brief One kind of per-object measurement, and the names it produces.
 *
 * Stored in an operation's parameters as its enumerator spelling, in a
 * comma-separated list (`"Area,Perimeter"`).
 * @par Requirements
 * UR-SEG-5
 */
enum class MeasurementKind : int32_t {
    Area,            ///< `area`, in square level-0 pixels.
    Perimeter,       ///< `perimeter`, the length of every ring, in level-0 pixels.
    Centroid,        ///< `centroid.x` and `centroid.y`, area-weighted and unrounded, in level-0 pixels.
    BoundingBox,     ///< `boundingBox.x`, `.y`, `.width` and `.height`, in level-0 pixels.
    IntensityMean,   ///< `intensity.<c>.mean` for each measured channel c.
    IntensityStdDev, ///< `intensity.<c>.stddev`, the population deviation.
    IntensityMin,    ///< `intensity.<c>.min`.
    IntensityMax     ///< `intensity.<c>.max`.
};

/**
 * @brief A set of MeasurementKind values: which measurements an operation
 * keeps on its objects.
 *
 * A kind is either in the set or not; adding it twice changes nothing. A
 * moved-from selection is empty.
 * @par Thread-safety
 * ThreadCompatible.
 * @par Requirements
 * UR-SEG-5
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS MeasurementSelection {
public:
    /**
     * @brief Creates an empty selection. Does not allocate.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-SEG-5
     */
    MeasurementSelection();
    /**
     * @brief Copies a selection; the copy shares nothing with the original.
     * @param other The selection to copy.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Reads @p other; ThreadCompatible.
     * @par Requirements
     * UR-SEG-5
     */
    MeasurementSelection(const MeasurementSelection& other);
    /**
     * @brief Moves a selection. The moved-from object is left empty.
     * @param other The selection to move from.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-5
     */
    MeasurementSelection(MeasurementSelection&& other) noexcept;
    /**
     * @brief Replaces this selection with a copy of another.
     * @param other The selection to copy.
     * @return This selection.
     * @throws std::bad_alloc When allocation fails; this selection is then
     * unchanged.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-5
     */
    MeasurementSelection& operator=(const MeasurementSelection& other);
    /**
     * @brief Replaces this selection by moving another. The moved-from object
     * is left empty.
     * @param other The selection to move from.
     * @return This selection.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-5
     */
    MeasurementSelection& operator=(MeasurementSelection&& other) noexcept;
    /**
     * @brief Destroys the selection.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-5
     */
    ~MeasurementSelection();

    /**
     * @brief Returns the selection of every kind.
     * @return All eight kinds.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-SEG-5
     */
    static MeasurementSelection standard();
    /**
     * @brief Returns the selection of the geometric kinds.
     * @return Area, Perimeter, Centroid and BoundingBox.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-SEG-5
     */
    static MeasurementSelection geometryOnly();

    /**
     * @brief Adds a kind; adding one already selected changes nothing.
     * @param kind The kind to add.
     * @return This selection, so calls can be chained.
     * @throws InvalidArgumentError When @p kind is not a MeasurementKind
     * enumerator; the selection is then unchanged.
     * @throws std::bad_alloc When this selection is empty and allocation
     * fails; it is then unchanged.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-5
     */
    MeasurementSelection& add(MeasurementKind kind);
    /**
     * @brief Adds every kind.
     * @return This selection, so calls can be chained.
     * @throws std::bad_alloc When this selection is empty and allocation
     * fails; it is then unchanged.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-5
     */
    MeasurementSelection& addAll();
    /**
     * @brief Tells whether a kind is selected.
     * @param kind The kind.
     * @return True when selected; false for a value that is not an enumerator.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-5
     */
    bool contains(MeasurementKind kind) const noexcept;
    /**
     * @brief Returns the number of kinds selected.
     * @return From 0 to 8.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-5
     */
    int32_t count() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_; // null while the selection is empty
};

/**
 * @brief Tells whether a scene's resolution gives a level-0 pixel size, so
 * that measurements are also given in micrometres.
 *
 * Calibration is applied automatically: a run over a calibrated scene adds the
 * micrometre names to every object it measures.
 * @param scene The scene; may be null.
 * @return True when scenePixelSizeMicrons() is positive; false for a null
 * scene.
 * @throws Nothing.
 * @par Thread-safety
 * Safe to call concurrently; reads the scene's metadata.
 * @par Requirements
 * UR-SEG-6
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS bool sceneIsCalibrated(std::shared_ptr<slideio::Scene> scene) noexcept;

/**
 * @brief Returns a scene's level-0 pixel size, the one a run measures with.
 * @param scene The scene; may be null.
 * @return The mean of the scene's x and y resolution in micrometres when both
 * are positive and agree to within one part in a million; 0 when either is
 * missing, the two disagree (anisotropic pixels), the scene is null or its
 * resolution cannot be read.
 * @throws Nothing.
 * @par Thread-safety
 * Safe to call concurrently; reads the scene's metadata.
 * @par Requirements
 * UR-SEG-6
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS double scenePixelSizeMicrons(std::shared_ptr<slideio::Scene> scene) noexcept;

} // namespace slideio::processor
