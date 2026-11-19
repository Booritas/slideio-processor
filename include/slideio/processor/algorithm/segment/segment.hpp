// Requirements: UR-SEG-1, UR-SEG-2, UR-SEG-3, UR-SEG-4, UR-SEG-5, UR-SEG-6, UR-SEG-7, UR-SEG-8
#pragma once

/**
 * @file segment.hpp
 * @brief Segmentation operations: Connectivity, the connected-components
 * labelling with its per-object measurements, the contours of a label raster,
 * the marker-controlled watershed that separates touching objects, and tissue
 * detection on RGB.
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
struct ContourParametersAccess;
struct WatershedParametersAccess;
struct TissueDetectParametersAccess;
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
 * UR-SEG-2, UR-SEG-4, UR-SEG-5, UR-SEG-7
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
     *
     * The intensity kinds are taken over the scene's samples of the channels
     * the operation's input planes 1 to 3 carry, as read, whatever the earlier
     * operations of the chain did to them (see the operation's factory).
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
 * @brief Creates `segment.connected_components`: Joint over one to four
 * planes, halo none, every input type, one `Int32` output plane, producing
 * objects.
 *
 * Plane 0 is segmented: a pixel is foreground when its sample is neither 0 nor
 * NaN. Each maximal set of foreground pixels connected under the chosen
 * Connectivity is one object, its outline and holes traced along pixel edges
 * in level-0 coordinates (UR-SEG-4); an object cut by tile seams is merged,
 * diagonal contacts across a seam included when 8-connected. Planes 1 to 3
 * are measured, so the run selects at most four channels, the first of them
 * segmented: validating a run that gives the operation more fails with
 * InvalidArgumentError, and so does its outputPlaneCount() asked for more
 * than four. An object's `intensity.<c>.mean`, `.stddev`, `.min` and `.max`
 * for each such plane's source channel c are over the object's own pixels,
 * NaN samples skipped and infinite ones counted. The geometric measurements
 * are `area`, `perimeter`, `centroid.x`, `centroid.y` and `boundingBox.x`,
 * `.y`, `.width`, `.height` in level-0 pixels, each also in micrometres, as
 * `<name>.um` or `area.um2`, when the scene is calibrated; only the kinds
 * selected are kept. An object whose final area lies outside the bounds is
 * discarded before it is stored, however it was assembled (UR-SEG-7).
 *
 * The measured samples are the scene's, as read: for each of input planes 1
 * onward, the selected channel it carries, through
 * OperationContext::sourceInput(), not the plane itself. A chain blurring and
 * thresholding before labelling segments the thresholded plane 0 and measures
 * the unblurred samples of the other channels, so `intensity.<c>.*` always
 * describes channel c as it is in the scene. A channel carried by several
 * input planes is measured once.
 * Derived planes (stain densities, HSV) are therefore never measured in the
 * same chain; to measure them, derive them in an inner pipeline and run this
 * operation over its Pipeline::asScene(), whose samples they then are.
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

/**
 * @brief The parameters of `segment.contours`, set one by one and passed to
 * makeContourExtraction().
 *
 * Each setter stores what the operation's parameter bag records under the key
 * of the same name, so the typed factory and the catalog build the same
 * operation from the same bag:
 * | Key | Type | Default | Range |
 * |---|---|---|---|
 * | `includeHoles` | bool | true | |
 * | `targetLayer` | string | absent: the run's only bound layer | the name of a bound layer |
 *
 * A moved-from object may only be assigned to or destroyed.
 * @par Thread-safety
 * ThreadCompatible.
 * @par Requirements
 * UR-SEG-2, UR-SEG-4
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS ContourParameters {
public:
    /**
     * @brief Creates the default parameters: holes kept, the run's only bound
     * layer.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-SEG-2
     */
    ContourParameters();
    /**
     * @brief Copies parameters; the copy shares nothing with the original.
     * @param other The parameters to copy; not moved-from.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Reads @p other; ThreadCompatible.
     * @par Requirements
     * UR-SEG-2
     */
    ContourParameters(const ContourParameters& other);
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
    ContourParameters(ContourParameters&& other) noexcept;
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
    ContourParameters& operator=(const ContourParameters& other);
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
    ContourParameters& operator=(ContourParameters&& other) noexcept;
    /**
     * @brief Destroys the parameters.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-2
     */
    ~ContourParameters();

    /**
     * @brief Sets whether an object keeps its holes as separate rings; true by
     * default. Without them, the object's area and perimeter are those of its
     * outer ring.
     * @param include True to keep the holes.
     * @return These parameters, so calls can be chained.
     * @throws InvalidArgumentError When these parameters are moved-from.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-4
     */
    ContourParameters& includeHoles(bool include);
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
    ContourParameters& targetLayer(const char* layerName);

private:
    friend struct detail::ContourParametersAccess;
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief Creates `segment.contours`: PerChannel, halo none, integer inputs up to
 * 32 bits and `Float32`, one `Int32` output plane per input plane, producing
 * objects.
 *
 * The input is a label raster. Each maximal 4-connected set of pixels of one
 * equal value, neither 0 nor NaN, is one object, its outline and holes traced
 * along pixel edges in level-0 coordinates (UR-SEG-4); `Float32` values are
 * compared exactly, so -0 is background as 0 is. Regions of different values
 * may touch: their objects then share the edges between them. An object cut
 * by tile seams is merged from its fragments, and only fragments of one value
 * are joined, so neighbouring regions stay apart across seams too. Two regions
 * of one value touching only at a corner are two objects.
 *
 * Nothing is measured but the geometry: each object has `area` and
 * `perimeter` in level-0 pixels, also as `area.um2` and `perimeter.um` when
 * the scene is calibrated. Without holes kept, they are those of the outer
 * ring.
 *
 * The operation runs on exactly one channel, the label raster: run per
 * channel over several, it would store every region once per channel under
 * one id, so validating a run that gives it more than one plane fails with
 * InvalidArgumentError.
 *
 * The output plane holds, for each pixel of an object, the object's 1-based
 * index among the objects of its tile in the order their first pixel is met
 * row by row, and 0 elsewhere. The index is local to the tile: it is a view
 * for inspection, not an object's identity.
 *
 * By name, an unknown key or a value of the wrong type throws
 * InvalidArgumentError naming the key, never the value.
 * @param parameters The parameters; not moved-from.
 * @return The operation, never null.
 * @throws InvalidArgumentError When @p parameters is moved-from; the message
 * names the operation and the parameter.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * Safe to call concurrently. The operation's apply() and finishObject() may
 * run concurrently on different contexts and objects.
 * @par Requirements
 * UR-SEG-2, UR-SEG-3, UR-SEG-4
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS OperationPtr makeContourExtraction(const ContourParameters& parameters);

/**
 * @brief Where `segment.watershed` takes its markers, the seeds its basins
 * grow from.
 *
 * Stored in the operation's parameters as its enumerator spelling,
 * `"LocalMaxima"` or `"AuxiliaryRaster"`.
 * @par Requirements
 * UR-SEG-8
 */
enum class WatershedMarkers : int32_t {
    /// Detected in plane 0: a foreground pixel greater than every other
    /// foreground pixel within `minimumMarkerDistancePx`, a plateau giving its
    /// topmost, then leftmost, pixel.
    LocalMaxima,
    /// One basin per distinct value other than -1 of an `Int64` raster bound
    /// to the run under `auxiliaryName`, normally
    /// LayerRasterContent::ObjectAnchor of a coarser stage's objects.
    AuxiliaryRaster
};

/**
 * @brief The parameters of `segment.watershed`, set one by one and passed to
 * makeWatershed().
 *
 * Each setter stores what the operation's parameter bag records under the key
 * of the same name, so the typed factory and the catalog build the same
 * operation from the same bag:
 * | Key | Type | Default | Range |
 * |---|---|---|---|
 * | `markers` | string | `"LocalMaxima"` | a WatershedMarkers spelling |
 * | `auxiliaryName` | string | `""` | with `AuxiliaryRaster`, the name of a bound raster, not empty |
 * | `minimumMarkerDistancePx` | int | 5 | 1 to `maximumMarkerSpacingPx` |
 * | `maximumMarkerSpacingPx` | int | 32 | 1 to 1073741823 |
 * | `minimumAreaPx` | int | 0 | at least 0 |
 * | `measurements` | string | every MeasurementKind | MeasurementKind spellings separated by commas, without spaces |
 * | `targetLayer` | string | absent: the run's only bound layer | the name of a bound layer |
 *
 * A moved-from object may only be assigned to or destroyed.
 * @par Thread-safety
 * ThreadCompatible.
 * @par Requirements
 * UR-SEG-2, UR-SEG-5, UR-SEG-7, UR-SEG-8
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS WatershedParameters {
public:
    /**
     * @brief Creates the default parameters: markers at local maxima at least
     * 5 pixels apart, a spacing of 32 pixels, no area bound, every
     * measurement, the run's only bound layer.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-SEG-8
     */
    WatershedParameters();
    /**
     * @brief Copies parameters; the copy shares nothing with the original.
     * @param other The parameters to copy; not moved-from.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Reads @p other; ThreadCompatible.
     * @par Requirements
     * UR-SEG-8
     */
    WatershedParameters(const WatershedParameters& other);
    /**
     * @brief Moves parameters. The moved-from object may only be assigned to
     * or destroyed.
     * @param other The parameters to move from.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-8
     */
    WatershedParameters(WatershedParameters&& other) noexcept;
    /**
     * @brief Replaces these parameters with a copy of others.
     * @param other The parameters to copy; not moved-from.
     * @return These parameters.
     * @throws std::bad_alloc When allocation fails; these parameters are then
     * unchanged.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-8
     */
    WatershedParameters& operator=(const WatershedParameters& other);
    /**
     * @brief Replaces these parameters by moving others. The moved-from object
     * may only be assigned to or destroyed.
     * @param other The parameters to move from.
     * @return These parameters.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-8
     */
    WatershedParameters& operator=(WatershedParameters&& other) noexcept;
    /**
     * @brief Destroys the parameters.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-8
     */
    ~WatershedParameters();

    /**
     * @brief Sets where the markers come from; `LocalMaxima` by default.
     * @param markers The source; checked by makeWatershed().
     * @return These parameters, so calls can be chained.
     * @throws InvalidArgumentError When these parameters are moved-from.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-8
     */
    WatershedParameters& markers(WatershedMarkers markers);
    /**
     * @brief Names the bound raster the markers are read from with
     * `AuxiliaryRaster`; empty by default, and ignored with `LocalMaxima`.
     * @param bindingName The binding's name, UTF-8; not null; copied.
     * @return These parameters, so calls can be chained.
     * @throws InvalidArgumentError When @p bindingName is null or these
     * parameters are moved-from.
     * @throws std::bad_alloc When allocation fails; these parameters are then
     * unchanged.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-8
     */
    WatershedParameters& auxiliaryName(const char* bindingName);
    /**
     * @brief Sets how far apart, in level pixels and Chebyshev distance, two
     * local maxima must be to be two markers; 5 by default. Used with
     * `LocalMaxima`.
     * @param distancePx The distance; checked by makeWatershed().
     * @return These parameters, so calls can be chained.
     * @throws InvalidArgumentError When these parameters are moved-from.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-8
     */
    WatershedParameters& minimumMarkerDistancePx(int32_t distancePx);
    /**
     * @brief Sets the reach of a basin, in level pixels and Chebyshev
     * distance; 32 by default.
     *
     * The bound that makes the operation tileable, and a hard one: every
     * foreground pixel must lie within this distance of the marker pixel its
     * basin reached it from. It derives the halo, 2 * spacing + 1. Exceeding
     * it fails the run; it is never approximated.
     *
     * Markers must lie within the spacing of every pixel, so the input
     * flooded should peak inside each object: on a plateau, and so on a
     * binary mask, only the topmost-leftmost pixels are markers, and the
     * spacing is usually exceeded. Flood a distance transform of the mask
     * instead.
     * @param spacingPx The distance; checked by makeWatershed().
     * @return These parameters, so calls can be chained.
     * @throws InvalidArgumentError When these parameters are moved-from.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-8
     */
    WatershedParameters& maximumMarkerSpacingPx(int32_t spacingPx);
    /**
     * @brief Sets the smallest area an object may have to be kept, in square
     * level-0 pixels, inclusive; 0 by default (no lower bound).
     * @param areaPx The bound; checked by makeWatershed().
     * @return These parameters, so calls can be chained.
     * @throws InvalidArgumentError When these parameters are moved-from.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-7
     */
    WatershedParameters& minimumAreaPx(int64_t areaPx);
    /**
     * @brief Sets which measurements each object keeps; every kind by default.
     *
     * The intensity kinds are taken over the scene's samples of the channels
     * the operation's input planes 1 to 3 carry, as read, whatever the earlier
     * operations of the chain did to them (see the operation's factory).
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
    WatershedParameters& measurements(const MeasurementSelection& selection);
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
    WatershedParameters& targetLayer(const char* layerName);

private:
    friend struct detail::WatershedParametersAccess;
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief Creates `segment.watershed`: Joint over one to four planes, halo
 * 2 * `maximumMarkerSpacingPx` + 1, every input type, one `Int32` output
 * plane, producing objects.
 *
 * Plane 0 is flooded: a pixel is foreground when its sample is neither 0 nor
 * NaN, and only foreground is flooded. Basins grow from the markers downhill,
 * through a frontier: the unflooded foreground pixels beside a flooded one.
 * The frontier pixel of highest value is flooded next, ties by level
 * coordinates (row, then column), and joins the basin of its highest flooded
 * side neighbour (ties again by level coordinates); a pixel joins the frontier
 * only once a neighbour is flooded, so this is not a sort of the whole tile.
 * The order never depends on where the tile lies. Samples are compared as
 * doubles, exactly for every type but 64-bit integers beyond 2^53 in
 * magnitude. Each basin's 4-connected
 * parts are its objects, traced along pixel edges in level-0 coordinates
 * (UR-SEG-4); basins touching each other share the edges between them and
 * stay apart across tile seams.
 *
 * Markers with `LocalMaxima` are the foreground pixels greater than every
 * other foreground pixel within `minimumMarkerDistancePx`, a pixel of equal
 * value counting as greater when it lies above, or to the left on the same
 * row; a plateau thus gives its topmost, then leftmost, pixel. With
 * `AuxiliaryRaster`, each foreground pixel whose value in the bound `Int64`
 * raster is not -1 is a marker, one basin per value; the raster covers the
 * same pixels as plane 0.
 *
 * **The reach is enforced.** A basin's boundary may depend only on markers
 * within `maximumMarkerSpacingPx`, which is what lets each tile decide it from
 * its own halo and keeps the result identical at every tile size (UR-SEG-3).
 * A foreground pixel no marker reaches, or reached from a marker pixel further
 * than the spacing, fails the tile, and so the run, with InvalidArgumentError
 * "segment.watershed: a region lies further than maximumMarkerSpacingPx from
 * every marker"; nothing is ever segmented partially in its place.
 *
 * Flood a surface that peaks inside each object, such as a distance transform
 * of a mask or a smoothed intensity. A plateau gives only its topmost-leftmost
 * pixels as markers, so a flat or binary input, a thresholded mask flooded as
 * it is, gives few markers far apart and usually fails the reach rather than
 * separating anything.
 *
 * Planes 1 to 3 are measured, so the run selects at most four channels, the
 * first of them flooded: validating a run that gives the operation more fails
 * with InvalidArgumentError, and so does its outputPlaneCount() asked for more
 * than four. The measurements are those of makeConnectedComponents(): `area`,
 * `perimeter`, `centroid.x`, `centroid.y`, `boundingBox.x`, `.y`, `.width`,
 * `.height` in level-0 pixels, each also in micrometres when the scene is
 * calibrated, and `intensity.<c>.mean`, `.stddev`, `.min` and `.max` per
 * measured channel c over the object's own pixels, NaN samples skipped and
 * infinite ones counted; only the kinds selected are kept. An object whose
 * final area is below `minimumAreaPx` is discarded before it is stored,
 * however it was assembled (UR-SEG-7). Holes are kept.
 *
 * The measured samples are the scene's, as read: for each of input planes 1
 * onward, the selected channel it carries, through
 * OperationContext::sourceInput(), not the plane itself. A chain smoothing
 * before flooding floods the smoothed plane 0 and measures the unsmoothed
 * samples of the other channels, so `intensity.<c>.*` always describes
 * channel c as it is in the scene. A channel carried by several input planes
 * is measured once.
 * Derived planes (stain densities, HSV) are therefore never measured in the
 * same chain; to measure them, derive them in an inner pipeline and run this
 * operation over its Pipeline::asScene(), whose samples they then are.
 *
 * The output plane holds, for each pixel of an object, the object's 1-based
 * index among the objects of its tile in the order their first pixel is met
 * row by row, and 0 elsewhere. The index is local to the tile: it is a view
 * for inspection, not an object's identity.
 *
 * The working memory is declared per pixel of the halo'd tile, which is large
 * beside the tile when the spacing is: a spacing of 32 grows a 256-pixel tile
 * to 386 pixels a side.
 *
 * By name, an unknown key, a value of the wrong type or a value out of range,
 * an unknown measurement kind among them, throws InvalidArgumentError naming
 * the key, never the value.
 * @param parameters The parameters; not moved-from.
 * @return The operation, never null.
 * @throws InvalidArgumentError When @p parameters is moved-from, the markers
 * are not an enumerator, `auxiliaryName` is empty with `AuxiliaryRaster`, a
 * distance is out of its range (the minimum marker distance above the spacing
 * included) or `minimumAreaPx` is negative; the message names the operation
 * and the parameter, never the value. A tile throws it when no raster is bound
 * under `auxiliaryName` or the bound one is not one `Int64` plane, naming the
 * parameter and never the binding, and when the reach is exceeded.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * Safe to call concurrently. The operation's apply() and finishObject() may
 * run concurrently on different contexts and objects.
 * @par Requirements
 * UR-SEG-2, UR-SEG-3, UR-SEG-4, UR-SEG-5, UR-SEG-7, UR-SEG-8
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS OperationPtr makeWatershed(const WatershedParameters& parameters);

/**
 * @brief The parameters of `segment.tissue_detect`, set one by one and passed
 * to makeTissueDetect().
 *
 * Each setter stores what the operation's parameter bag records under the key
 * of the same name, so the typed factory and the catalog build the same
 * operation from the same bag:
 * | Key | Type | Default | Range |
 * |---|---|---|---|
 * | `saturationThreshold` | double | 0.07 | 0 to 1 |
 * | `minimumRegionAreaPx` | int | 0 | at least 0 |
 * | `closingRadiusPx` | int | 2 | 0 to 127 |
 * | `targetLayer` | string | absent: the run's only bound layer | the name of a bound layer |
 *
 * A moved-from object may only be assigned to or destroyed.
 * @par Thread-safety
 * ThreadCompatible.
 * @par Requirements
 * UR-SEG-1, UR-SEG-7
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS TissueDetectParameters {
public:
    /**
     * @brief Creates the default parameters: a saturation threshold of 0.07,
     * no area bound, a closing radius of 2 pixels, the run's only bound layer.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-SEG-1
     */
    TissueDetectParameters();
    /**
     * @brief Copies parameters; the copy shares nothing with the original.
     * @param other The parameters to copy; not moved-from.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Reads @p other; ThreadCompatible.
     * @par Requirements
     * UR-SEG-1
     */
    TissueDetectParameters(const TissueDetectParameters& other);
    /**
     * @brief Moves parameters. The moved-from object may only be assigned to
     * or destroyed.
     * @param other The parameters to move from.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-1
     */
    TissueDetectParameters(TissueDetectParameters&& other) noexcept;
    /**
     * @brief Replaces these parameters with a copy of others.
     * @param other The parameters to copy; not moved-from.
     * @return These parameters.
     * @throws std::bad_alloc When allocation fails; these parameters are then
     * unchanged.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-1
     */
    TissueDetectParameters& operator=(const TissueDetectParameters& other);
    /**
     * @brief Replaces these parameters by moving others. The moved-from object
     * may only be assigned to or destroyed.
     * @param other The parameters to move from.
     * @return These parameters.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-1
     */
    TissueDetectParameters& operator=(TissueDetectParameters&& other) noexcept;
    /**
     * @brief Destroys the parameters.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-1
     */
    ~TissueDetectParameters();

    /**
     * @brief Sets the saturation above which a pixel is tissue; 0.07 by
     * default.
     * @param threshold The threshold, from 0 to 1; checked by
     * makeTissueDetect().
     * @return These parameters, so calls can be chained.
     * @throws InvalidArgumentError When these parameters are moved-from.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-1
     */
    TissueDetectParameters& saturationThreshold(double threshold);
    /**
     * @brief Sets the smallest area a tissue region may have to be kept, in
     * square level-0 pixels, inclusive; 0 by default (no bound).
     * @param areaPx The bound; checked by makeTissueDetect().
     * @return These parameters, so calls can be chained.
     * @throws InvalidArgumentError When these parameters are moved-from.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-7
     */
    TissueDetectParameters& minimumRegionAreaPx(int64_t areaPx);
    /**
     * @brief Sets the radius, in pixels of the analysis level, of the disc the
     * tissue mask is closed with; 2 by default, and 0 for no closing.
     *
     * The radius derives the halo, 2 * radius + 1.
     * @param radiusPx The radius, from 0 to 127; checked by makeTissueDetect().
     * @return These parameters, so calls can be chained.
     * @throws InvalidArgumentError When these parameters are moved-from.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-SEG-1
     */
    TissueDetectParameters& closingRadiusPx(int32_t radiusPx);
    /**
     * @brief Names the bound layer the tissue regions go to.
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
    TissueDetectParameters& targetLayer(const char* layerName);

private:
    friend struct detail::TissueDetectParametersAccess;
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief Creates `segment.tissue_detect`: Joint over three planes of one type,
 * `Byte` or `UInt16`, halo 2 * `closingRadiusPx` + 1, one `Byte` output plane,
 * producing objects.
 *
 * The three planes are a pixel's red, green and blue samples, in any order. A
 * pixel is tissue when its saturation, (max - min) / max over its three
 * samples, exceeds `saturationThreshold`; a pixel whose samples are all 0,
 * as pixels beyond the level are, is background. The tissue mask is then
 * closed, dilated and then eroded, with a disc of radius `closingRadiusPx` in
 * pixels of the analysis level: the pixels within that Euclidean distance of
 * its centre. The closing bridges gaps and fills holes narrower than the
 * disc, and is exact under tiling: the halo holds every pixel a tile pixel's
 * closed value depends on. Each maximal 4-connected set of closed tissue
 * pixels is one object, its outline and holes traced along pixel edges in
 * level-0 coordinates (UR-SEG-4); an object cut by tile seams is merged from
 * its fragments.
 *
 * Nothing is measured but the geometry: each object has `area` and
 * `perimeter` in level-0 pixels, also as `area.um2` and `perimeter.um` when
 * the scene is calibrated. Holes are kept. An object whose final area is below
 * `minimumRegionAreaPx` is discarded before it is stored, however it was
 * assembled (UR-SEG-7); the area is in level-0 pixels, so the bound means the
 * same at every analysis level.
 *
 * The run selects exactly three channels: validating a run that gives the
 * operation another number fails with ChannelArityError, and so does its
 * outputPlaneCount() asked for another number.
 *
 * The output plane holds 255 for each closed tissue pixel of the tile and 0
 * elsewhere.
 *
 * **Run it at a coarse analysis level.** Tissue does not need full resolution
 * (PipelineSpec::analysisLevel), and one tissue region may span the level:
 * the run holds its seam fragments until the region is complete, so the
 * memory they take grows with the region's outline at the analysis level.
 *
 * By name, an unknown key, a value of the wrong type or a value out of range
 * throws InvalidArgumentError naming the key, never the value.
 * @param parameters The parameters; not moved-from.
 * @return The operation, never null.
 * @throws InvalidArgumentError When @p parameters is moved-from or a value is
 * out of its range (the threshold not from 0 to 1, a negative area bound, the
 * radius not from 0 to 127); the message names the operation and the
 * parameter, never the value.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * Safe to call concurrently. The operation's apply() and finishObject() may
 * run concurrently on different contexts and objects.
 * @par Requirements
 * UR-SEG-1, UR-SEG-2, UR-SEG-3, UR-SEG-4, UR-SEG-7
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS OperationPtr makeTissueDetect(const TissueDetectParameters& parameters);

} // namespace slideio::processor
