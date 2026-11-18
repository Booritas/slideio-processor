// Requirements: UR-SEG-2, UR-SEG-3, UR-SEG-4, UR-SEG-5, UR-SEG-7
#pragma once

/**
 * @file segment.hpp
 * @brief Segmentation operations: Connectivity and the connected-components
 * labelling with its per-object measurements.
 *
 * A segmentation operation writes objects to a layer of the run. Each object is
 * found on the tile it lies in; one cut by tile seams is emitted in fragments
 * the run merges, so its geometry and measurements are those a single tile
 * covering the level would give. Every object, whole or merged, is then
 * finished by its operation: holes kept or removed, area filters applied and
 * the selected measurements kept, in level-0 pixels and, when the scene is
 * calibrated, in micrometres. Areas are in square level-0 pixels at every
 * analysis level.
 */

#include <slideio/processor/algorithm/measure/measure.hpp>
#include <slideio/processor/pipeline/export.hpp>
#include <slideio/processor/pipeline/operation.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

namespace detail {
struct ConnectedComponentsParametersAccess;
} // namespace detail

/**
 * @brief Which neighbours of a pixel belong to its region.
 *
 * Stored in an operation's parameters as its enumerator spelling,
 * `"FourConnected"` or `"EightConnected"`.
 * @par Requirements
 * UR-SEG-2
 */
enum class Connectivity : int32_t {
    FourConnected, ///< The pixels sharing a side.
    EightConnected ///< The pixels sharing a side or a corner.
};

/**
 * @brief The parameters of `segment.connected_components`, set one by one and
 * passed to makeConnectedComponents().
 *
 * Each setter stores what the operation's parameter bag records under the key
 * of the same name, so the typed factory and the catalog build the same
 * operation from the same bag:
 * | Key | Type | Default | Range |
 * |---|---|---|---|
 * | `connectivity` | string | `"EightConnected"` | a Connectivity spelling |
 * | `minimumAreaPx` | int | 0 | at least 0 |
 * | `maximumAreaPx` | int | 0 (unbounded) | at least 0; when not 0, at least `minimumAreaPx` |
 * | `extractHoles` | bool | true | |
 * | `measurements` | string | every MeasurementKind | MeasurementKind spellings separated by commas, without spaces |
 * | `targetLayer` | string | absent: the run's only bound layer | the name of a bound layer |
 *
 * A moved-from object may only be assigned to or destroyed.
 * @par Thread-safety
 * ThreadCompatible.
 * @par Requirements
 * UR-SEG-2, UR-SEG-7
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS ConnectedComponentsParameters {
public:
    /**
     * @brief Creates the default parameters: 8-connected, no area bounds,
     * holes kept, every measurement, the run's only bound layer.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-SEG-2
     */
    ConnectedComponentsParameters();
    /**
     * @brief Copies parameters; the copy shares nothing with the original.
     * @param other The parameters to copy; not moved-from.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Reads @p other; ThreadCompatible.
     * @par Requirements
     * UR-SEG-2
     */
    ConnectedComponentsParameters(const ConnectedComponentsParameters& other);
    /**
     * @brief Moves parameters. The moved-from object may only be assigned to
     * or destroyed.
     * @param other The parameters to move from.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-2
     */
    ConnectedComponentsParameters(ConnectedComponentsParameters&& other) noexcept;
    /**
     * @brief Replaces these parameters with a copy of others.
     * @param other The parameters to copy; not moved-from.
     * @return These parameters.
     * @throws std::bad_alloc When allocation fails; these parameters are then
     * unchanged.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-2
     */
    ConnectedComponentsParameters& operator=(const ConnectedComponentsParameters& other);
    /**
     * @brief Replaces these parameters by moving others. The moved-from object
     * may only be assigned to or destroyed.
     * @param other The parameters to move from.
     * @return These parameters.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-2
     */
    ConnectedComponentsParameters& operator=(ConnectedComponentsParameters&& other) noexcept;
    /**
     * @brief Destroys the parameters.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-2
     */
    ~ConnectedComponentsParameters();

    /**
     * @brief Sets which neighbours join a region; `EightConnected` by default.
     * @param connectivity The connectivity; checked by makeConnectedComponents().
     * @return These parameters, so calls can be chained.
     * @throws InvalidArgumentError When these parameters are moved-from.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-2
     */
    ConnectedComponentsParameters& connectivity(Connectivity connectivity);
    /**
     * @brief Sets the smallest area an object may have to be kept, in square
     * level-0 pixels, inclusive; 0 by default (no lower bound).
     * @param areaPx The bound; checked by makeConnectedComponents().
     * @return These parameters, so calls can be chained.
     * @throws InvalidArgumentError When these parameters are moved-from.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-7
     */
    ConnectedComponentsParameters& minimumAreaPx(int64_t areaPx);
    /**
     * @brief Sets the largest area an object may have to be kept, in square
     * level-0 pixels, inclusive; 0 by default (no upper bound).
     * @param areaPx The bound, or 0; checked by makeConnectedComponents().
     * @return These parameters, so calls can be chained.
     * @throws InvalidArgumentError When these parameters are moved-from.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-7
     */
    ConnectedComponentsParameters& maximumAreaPx(int64_t areaPx);
    /**
     * @brief Sets whether an object keeps its holes as separate rings; true by
     * default. Without them, the object's area and perimeter are those of its
     * outer ring, and its intensity statistics stay over its own pixels.
     * @param extract True to keep the holes.
     * @return These parameters, so calls can be chained.
     * @throws InvalidArgumentError When these parameters are moved-from.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-4
     */
    ConnectedComponentsParameters& extractHoles(bool extract);
    /**
     * @brief Sets which measurements each object keeps; every kind by default.
     * @param selection The kinds; may be empty, which keeps none.
     * @return These parameters, so calls can be chained.
     * @throws InvalidArgumentError When these parameters are moved-from.
     * @throws std::bad_alloc When allocation fails; these parameters are then
     * unchanged.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-5
     */
    ConnectedComponentsParameters& measurements(const MeasurementSelection& selection);
    /**
     * @brief Names the bound layer the objects go to.
     *
     * An empty name, the default, leaves the key out of the parameter bag, and
     * the run then uses its only bound layer; a run binding several layers
     * needs a name.
     * @param layerName The layer's name, UTF-8; not null; copied.
     * @return These parameters, so calls can be chained.
     * @throws InvalidArgumentError When @p layerName is null or these
     * parameters are moved-from.
     * @throws std::bad_alloc When allocation fails; these parameters are then
     * unchanged.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-2
     */
    ConnectedComponentsParameters& targetLayer(const char* layerName);

private:
    friend struct detail::ConnectedComponentsParametersAccess;
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief Creates `segment.connected_components`: Joint, halo 1, every input
 * type, one `Int32` output plane, producing objects.
 *
 * Plane 0 is segmented: a pixel is foreground when its sample is neither 0 nor
 * NaN. Each maximal set of foreground pixels connected under the chosen
 * Connectivity is one object, its outline and holes traced along pixel edges
 * in level-0 coordinates (UR-SEG-4); an object cut by tile seams is merged,
 * diagonal contacts across a seam included when 8-connected. Planes 1 and on
 * are measured: an object's `intensity.<c>.mean`, `.stddev`, `.min` and `.max`
 * for each such plane's source channel c are over the object's own pixels,
 * NaN samples skipped and infinite ones counted. The geometric measurements
 * are `area`, `perimeter`, `centroid.x`, `centroid.y` and `boundingBox.x`,
 * `.y`, `.width`, `.height` in level-0 pixels, each also in micrometres, as
 * `<name>.um` or `area.um2`, when the scene is calibrated; only the kinds
 * selected are kept. An object whose final area lies outside the bounds is
 * discarded before it is stored, however it was assembled (UR-SEG-7).
 *
 * The output plane holds, for each pixel of an object, the object's 1-based
 * index among the objects of its tile in the order their first pixel is met
 * row by row, and 0 elsewhere. The index is local to the tile: it is a view
 * for inspection, not an object's identity.
 *
 * By name, an unknown key, a value of the wrong type or a value out of range,
 * an unknown measurement kind among them, throws InvalidArgumentError naming
 * the key, never the value.
 * @param parameters The parameters; not moved-from.
 * @return The operation, never null.
 * @throws InvalidArgumentError When @p parameters is moved-from, the
 * connectivity is not an enumerator, an area bound is negative, or
 * `maximumAreaPx` is neither 0 nor at least `minimumAreaPx`; the message names
 * the operation and the parameter, never the value.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * Safe to call concurrently. The operation's apply() and finishObject() may
 * run concurrently on different contexts and objects.
 * @par Requirements
 * UR-SEG-2, UR-SEG-3, UR-SEG-4, UR-SEG-5, UR-SEG-6, UR-SEG-7
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS OperationPtr makeConnectedComponents(const ConnectedComponentsParameters& parameters);

} // namespace slideio::processor
