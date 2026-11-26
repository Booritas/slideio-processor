// Requirements: UR-PIX-3, UR-PIX-4, UR-PIX-7, UR-PIX-8, UR-PIX-9, UR-EXT-3, UR-QA-2, UR-IMG-11, UR-ALG-6, UR-ALG-8
#pragma once

/**
 * @file filter.hpp
 * @brief Neighbourhood filters and morphology: BorderMode, the Gaussian,
 * median, Sobel, Scharr, Laplacian, Canny and bilateral filters, the distance
 * transform, hole filling, removal of large objects, StructuringShape, StructuringElement, and erode, dilate, open and
 * close.
 *
 * Every operation here is PerChannel with a halo derived from its
 * parameters. The framework supplies exactly that border context, reading
 * pixels beyond the level as zero, so each operation except Canny produces
 * the same whole-slide output at any tile size as one tile covering the level.
 */

#include <slideio/processor/pipeline/export.hpp>
#include <slideio/processor/pipeline/operation.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

namespace detail {
struct StructuringElementAccess;
} // namespace detail

/**
 * @brief How a filter reads pixels beyond the image's border.
 *
 * No factory takes a BorderMode in this version: the framework supplies each
 * operation's border context itself, and pixels beyond the level read as
 * zero, as with `Constant` and a value of 0. The enumeration is declared so a
 * later version can expose the choice without changing this header's other
 * declarations.
 * @par Requirements
 * UR-PIX-3
 */
enum class BorderMode : int32_t {
    Reflect,   ///< The image mirrored about its edge.
    Replicate, ///< The edge pixel repeated.
    Constant   ///< A constant value.
};

/**
 * @brief Creates `pixel.gaussian_blur`: PerChannel, halo kernelSize / 2 on
 * every side, every input type except `Int64` and `UInt64`, output `Float32`,
 * or `Float64` for `Float64` input.
 *
 * Each output sample is the input convolved with the separable kernel
 * exp(-d^2 / (2 @p sigma^2)) for d from -kernelSize / 2 to kernelSize / 2,
 * normalised to sum 1, the same kernel horizontally and vertically. A
 * @p kernelSize of 0 derives the size 2 * ceil(3 * @p sigma) + 1; a size of 1
 * leaves every sample unchanged apart from the conversion to the output type.
 * Pixels beyond the level read as zero. Every pixel is computed from its own
 * window alone, in double and in one fixed order: for each window row top to
 * bottom its weighted sum left to right, then those row sums weighted top to
 * bottom. The result does not depend on the tiling, and a `Float32` result is
 * the double rounded to nearest. The 64-bit integer types are refused because
 * their samples are not all exact in double.
 * A NaN in a pixel's window makes it NaN; an infinity makes it +inf or -inf,
 * or NaN where it meets a zero coefficient or an infinity of the opposite
 * sign.
 *
 * Its parameters are `sigma` (a double; 1.0 when created by name without it)
 * and `kernelSize` (an integer; 0 by default), stored as given, and
 * Operation::parameters() always holds both. Creating it by name through
 * OperationCatalog::create with the same values gives equal parameters, and an
 * integer `sigma` is read as the equal double. By name, an unknown key, a
 * value of the wrong type or a value out of range throws InvalidArgumentError
 * naming the key, never the value.
 * @param sigma The standard deviation in pixels; finite and at least 1e-6,
 * below which its square may underflow. With @p kernelSize 0 the derived size
 * must not exceed 1025.
 * @param kernelSize The kernel's width and height: odd, from 1 to 1025, or 0
 * to derive it from @p sigma.
 * @return The operation, never null.
 * @throws InvalidArgumentError When @p sigma is not finite or below 1e-6, or
 * derives a size above 1025, or @p kernelSize is even and non-zero,
 * negative or above 1025; the message names the operation and the parameter,
 * never the value.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * Safe to call concurrently. The operation's apply() may run concurrently on
 * different contexts.
 * @par Requirements
 * UR-PIX-3, UR-QA-2, UR-IMG-11, UR-ALG-6
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS OperationPtr makeGaussianBlur(double sigma, int32_t kernelSize);

/**
 * @brief Creates `pixel.median_blur`: PerChannel, halo kernelSize / 2 on every
 * side, every input type, output the input type.
 *
 * Each output sample is the median of the @p kernelSize x @p kernelSize window
 * centred on it: the middle value of the window's samples sorted, an existing
 * sample, so no value is created and the type is kept. Integers are compared
 * exactly; a NaN sorts above every number. Pixels beyond the level read as
 * zero.
 *
 * Its parameter is `kernelSize` (an integer; 3 when created by name without
 * it), and Operation::parameters() always holds it. Creating it by name with
 * the same value gives equal parameters. By name, an unknown key, a value of
 * the wrong type or a value out of range throws InvalidArgumentError naming
 * the key, never the value.
 * @param kernelSize The window's width and height; odd, from 3 to 255.
 * @return The operation, never null.
 * @throws InvalidArgumentError When @p kernelSize is even or outside 3 to 255;
 * the message names the operation and the parameter, never the value.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * Safe to call concurrently. The operation's apply() may run concurrently on
 * different contexts.
 * @par Requirements
 * UR-PIX-3, UR-QA-2, UR-IMG-11, UR-ALG-6
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS OperationPtr makeMedianBlur(int32_t kernelSize);

/**
 * @brief Creates `pixel.distance_transform`: PerChannel, halo
 * @p maximumDistancePx on every side, every input type, output `Float32`.
 *
 * A pixel is foreground when its sample is neither 0 nor NaN (a negative
 * sample is foreground), the rule `segment.watershed` uses. Each output sample
 * is 0 for a background pixel, and for a foreground pixel the Euclidean
 * distance, in pixels, to the centre of the nearest background pixel, clamped
 * at @p maximumDistancePx. Pixels beyond the level are zero, so they are
 * background: an object at the level edge gets its distance to that edge. The
 * distance is exact: the module's own Felzenszwalb–Huttenlocher transform
 * finds the integer squared distance in integer arithmetic alone, and the
 * output is its square root as float. The halo is the clamp, so the result is
 * bitwise the same at every tile size. Its natural use is the input of
 * `segment.watershed`.
 *
 * Its parameter is `maximumDistancePx` (an integer; 32 when created by name
 * without it), and Operation::parameters() always holds it. Creating it by
 * name with the same value gives equal parameters. By name, an unknown key, a
 * value of the wrong type or a value out of range throws InvalidArgumentError
 * naming the key, never the value.
 * @param maximumDistancePx The clamp and the halo, in pixels; from 1 to 4096.
 * @return The operation, never null.
 * @throws InvalidArgumentError When @p maximumDistancePx is outside 1 to 4096;
 * the message names the operation and the parameter, never the value.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * Safe to call concurrently. The operation's apply() may run concurrently on
 * different contexts.
 * @par Requirements
 * UR-PIX-7, UR-QA-2, UR-ALG-6
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS OperationPtr makeDistanceTransform(int32_t maximumDistancePx);

/**
 * @brief Creates `pixel.fill_holes`: PerChannel, halo @p maximumHoleSizePx on
 * every side, every input type, output `Byte`.
 *
 * A pixel is foreground when its sample is neither 0 nor NaN, the rule
 * `segment.watershed` uses. The output is 255 for a foreground pixel and for a
 * pixel of a hole, 0 elsewhere. A hole is a 4-connected component of
 * background (the usual pair with an 8-connected foreground) that does not
 * reach the level edge, whose bounding box is at most @p maximumHoleSizePx on
 * each side, and, when @p maximumHoleAreaPx is given, whose area is at most
 * that. Pixels beyond the level are zero, so a region open to the level edge
 * is not a hole. A hole within the size lies whole inside the halo'd context,
 * and one that does not is seen touching the context's border or too large, so
 * the result does not depend on the tiling.
 *
 * Its parameters are `maximumHoleSizePx` (an integer; 32 when created by name
 * without it) and `maximumHoleAreaPx` (an integer of at least 1, absent by
 * default: no area limit). Operation::parameters() always holds the size and
 * holds the area only when one is set. Creating it by name with the same
 * values gives equal parameters. By name, an unknown key, a value of the wrong
 * type or a value out of range throws InvalidArgumentError naming the key,
 * never the value.
 * @param maximumHoleSizePx The size bound and the halo, in pixels; from 1 to
 * 4096.
 * @param maximumHoleAreaPx The area bound in pixels, at least 1; 0 means no
 * area limit and leaves the key absent.
 * @return The operation, never null.
 * @throws InvalidArgumentError When @p maximumHoleSizePx is outside 1 to 4096
 * or @p maximumHoleAreaPx is negative; the message names the operation and the
 * parameter, never the value. A tile throws it, failing the run, when the tile
 * grown by its halo on every side has more than 2147483647 pixels, which it
 * cannot label; the message gives that count and asks for a smaller tile size.
 * The run's planning does not see this, as for `segment.watershed` and the
 * other labelling operations, so tiles may already have been processed.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * Safe to call concurrently. The operation's apply() may run concurrently on
 * different contexts.
 * @par Requirements
 * UR-PIX-8, UR-QA-2, UR-ALG-6
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS OperationPtr makeFillHoles(int32_t maximumHoleSizePx, int64_t maximumHoleAreaPx = 0);

/**
 * @brief Creates `pixel.remove_large_objects`: PerChannel, halo
 * @p maximumObjectSizePx on every side, every input type, output `Byte`.
 *
 * A pixel is foreground when its sample is neither 0 nor NaN, the rule
 * `segment.watershed` uses. An object is an 8-connected component of
 * foreground (the usual pair with `pixel.fill_holes`' 4-connected background).
 * The output is 255 for a foreground pixel whose object's bounding box is at
 * most @p maximumObjectSizePx wide and at most @p maximumObjectSizePx high, 0
 * elsewhere: an object wider or higher than the bound is removed. Pixels
 * beyond the level are zero, so an object cut by the level edge is measured by
 * its pixels inside the level. An object that touches the halo'd context's
 * border reaches at least the halo from a tile pixel, so it is wider or higher
 * than the bound and is removed; every other object lies whole inside the
 * context and is measured exactly. So the result does not depend on the
 * tiling.
 *
 * Removal takes the whole 8-connected object: a nucleus that touches a large
 * stained area, even diagonally, is removed with it. A cut-off that merges
 * nuclei into large regions loses them silently, so check the threshold, for
 * example by comparing object counts with and without this step.
 *
 * Its one parameter is `maximumObjectSizePx` (an integer; 32 when created by
 * name without it). Operation::parameters() holds exactly that. Creating it by
 * name with the same value gives equal parameters. By name, an unknown key, a
 * value of the wrong type or a value out of range throws InvalidArgumentError
 * naming the key, never the value.
 * @param maximumObjectSizePx The size bound and the halo, in pixels; from 1 to
 * 4096.
 * @return The operation, never null.
 * @throws InvalidArgumentError When @p maximumObjectSizePx is outside 1 to 4096;
 * the message names the operation and the parameter, never the value. A tile
 * throws it, failing the run, when the tile grown by its halo on every side has
 * more than 2147483647 pixels, which it cannot label; the message gives that
 * count and asks for a smaller tile size. The run's planning does not see this,
 * as for `pixel.fill_holes` and the other labelling operations, so tiles may
 * already have been processed.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * Safe to call concurrently. The operation's apply() may run concurrently on
 * different contexts.
 * @par Requirements
 * UR-PIX-9, UR-QA-3, UR-ALG-6
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS OperationPtr makeRemoveLargeObjects(int32_t maximumObjectSizePx);

/**
 * @brief Creates `pixel.sobel`: PerChannel, halo max(1, kernelSize / 2) on
 * every side, every input type except `Int64` and `UInt64`, output `Float32`,
 * or `Float64` for `Float64` input.
 *
 * Each output sample is the @p dx -th derivative in x and @p dy -th in y,
 * by the separable, unnormalised Sobel coefficients OpenCV's `Sobel` uses for
 * the same orders and size. A @p kernelSize of 1 differentiates with a 3-tap
 * kernel along the derivative's axis and does not smooth across it. Pixels
 * beyond the level read as zero. Computed in double in one fixed order, each
 * window row's weighted sum left to right, then those sums top to bottom, so
 * the result does not depend on the tiling.
 * A NaN in a pixel's window makes it NaN; an infinity makes it +inf or -inf,
 * or NaN where it meets a zero coefficient or an infinity of the opposite
 * sign.
 *
 * Its parameters are `dx`, `dy` and `kernelSize` (integers; 1, 0 and 3 when
 * created by name without them), and Operation::parameters() always holds
 * all three. Creating it by name with the same values gives equal parameters.
 * By name, an unknown key, a value of the wrong type or a value out of range
 * throws InvalidArgumentError naming the key, never the value.
 * @param dx The derivative's order in x; 0, 1 or 2.
 * @param dy The derivative's order in y; 0, 1 or 2. @p dx + @p dy must be at
 * least 1.
 * @param kernelSize 1, 3, 5 or 7.
 * @return The operation, never null.
 * @throws InvalidArgumentError When an order is outside 0 to 2, both are 0,
 * or @p kernelSize is not 1, 3, 5 or 7; the message names the operation and
 * the parameter, never the value.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * Safe to call concurrently. The operation's apply() may run concurrently on
 * different contexts.
 * @par Requirements
 * UR-PIX-3, UR-QA-2, UR-IMG-11, UR-ALG-6
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS OperationPtr makeSobel(int32_t dx, int32_t dy, int32_t kernelSize);

/**
 * @brief Creates `pixel.scharr`: PerChannel, halo 1 on every side, every
 * input type except `Int64` and `UInt64`, output `Float32`, or `Float64` for
 * `Float64` input.
 *
 * Each output sample is the first derivative in x or in y by the separable,
 * unnormalised 3 x 3 Scharr coefficients OpenCV uses. Pixels beyond the level
 * read as zero. Computed in double in one fixed order, so the result does not
 * depend on the tiling.
 * A NaN in a pixel's window makes it NaN; an infinity makes it +inf or -inf,
 * or NaN where it meets a zero coefficient or an infinity of the opposite
 * sign.
 *
 * Its parameters are `dx` and `dy` (integers; 1 and 0 when created by name
 * without them), and Operation::parameters() always holds both. Creating it
 * by name with the same values gives equal parameters. By name, an unknown
 * key, a value of the wrong type or a value out of range throws
 * InvalidArgumentError naming the key, never the value.
 * @param dx The derivative's order in x.
 * @param dy The derivative's order in y. (@p dx, @p dy) must be (1, 0) or
 * (0, 1).
 * @return The operation, never null.
 * @throws InvalidArgumentError When (@p dx, @p dy) is neither (1, 0) nor
 * (0, 1); the message names the operation and the parameter, never the value.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * Safe to call concurrently. The operation's apply() may run concurrently on
 * different contexts.
 * @par Requirements
 * UR-PIX-3, UR-QA-2, UR-IMG-11, UR-ALG-6
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS OperationPtr makeScharr(int32_t dx, int32_t dy);

/**
 * @brief Creates `pixel.laplacian`: PerChannel, halo max(1, kernelSize / 2)
 * on every side, every input type except `Int64` and `UInt64`, output
 * `Float32`, or `Float64` for `Float64` input.
 *
 * Each output sample is the second derivative in x plus the second derivative
 * in y, each a separable pass computed in double in one fixed order, the x
 * term first. A @p kernelSize of 1 is the 3 x 3 kernel
 * [[0, 1, 0], [1, -4, 1], [0, 1, 0]], the second differences [1, -2, 1] in x
 * and in y; a larger size uses the unnormalised Sobel second-derivative
 * coefficients of that size. Pixels beyond the level read as zero. The result
 * does not depend on the tiling.
 * A NaN in a pixel's window makes it NaN; an infinity makes it +inf or -inf,
 * or NaN where it meets a zero coefficient or an infinity of the opposite
 * sign.
 *
 * Its parameter is `kernelSize` (an integer; 1 when created by name without
 * it), and Operation::parameters() always holds it. Creating it by name with
 * the same value gives equal parameters. By name, an unknown key, a value of
 * the wrong type or a value out of range throws InvalidArgumentError naming
 * the key, never the value.
 * @param kernelSize 1, 3, 5 or 7.
 * @return The operation, never null.
 * @throws InvalidArgumentError When @p kernelSize is not 1, 3, 5 or 7; the
 * message names the operation and the parameter, never the value.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * Safe to call concurrently. The operation's apply() may run concurrently on
 * different contexts.
 * @par Requirements
 * UR-PIX-3, UR-QA-2, UR-IMG-11, UR-ALG-6
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS OperationPtr makeLaplacian(int32_t kernelSize);

/**
 * @brief Creates `pixel.canny`: PerChannel, halo apertureSize / 2 + 1 + 32 on
 * every side, input `Byte`, output `Byte`: 255 on an edge, 0 elsewhere.
 *
 * Canny's edge detector with the L1 gradient magnitude: Sobel gradients of
 * aperture @p apertureSize, non-maximum suppression, and hysteresis, in which
 * a pixel whose magnitude exceeds @p highThreshold is a strong edge and one
 * above @p lowThreshold is kept when a chain of such pixels links it to a
 * strong edge. Pixels beyond the level read as zero.
 *
 * Unlike every other operation here, the halo is a stated bound rather than a
 * guarantee. The gradient and suppression need apertureSize / 2 + 1 pixels;
 * the remaining 32 are the reach allowed to hysteresis, whose chains are of
 * unbounded length. The result equals single-tile Canny wherever every weak
 * edge chain reaches a strong edge within 32 pixels, and may drop or keep weak
 * edges beyond that reach: a tile may lack a chain's strong edge, and the
 * gradients OpenCV computes at the halo's outer edge, past which it replicates
 * the border, may differ from the level's and make a strong edge the level
 * does not have. The operation's complexity note states this bound.
 *
 * Its parameters are `lowThreshold` and `highThreshold` (doubles; 50.0 and
 * 150.0 when created by name without them) and `apertureSize` (an integer; 3
 * by default), and Operation::parameters() always holds all three. Creating
 * it by name with the same values gives equal parameters. By name, an unknown
 * key, a value of the wrong type or a value out of range throws
 * InvalidArgumentError naming the key, never the value.
 * @param lowThreshold The hysteresis threshold for weak edges; finite, at
 * least 0.
 * @param highThreshold The threshold for strong edges; finite, at least
 * @p lowThreshold.
 * @param apertureSize The Sobel aperture; 3, 5 or 7.
 * @return The operation, never null.
 * @throws InvalidArgumentError When a threshold is not finite, @p lowThreshold
 * is negative or above @p highThreshold, or @p apertureSize is not 3, 5 or 7;
 * the message names the operation and the parameter, never the value.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * Safe to call concurrently. The operation's apply() may run concurrently on
 * different contexts.
 * @par Requirements
 * UR-PIX-3, UR-IMG-11, UR-ALG-6, UR-ALG-8
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS OperationPtr makeCanny(double lowThreshold, double highThreshold,
                                                        int32_t apertureSize);

/**
 * @brief Creates `pixel.bilateral_filter`: PerChannel, halo diameter / 2 on
 * every side, every input type except `Int64` and `UInt64`, output `Float32`,
 * or `Float64` for `Float64` input.
 *
 * Each output sample is the weighted mean of the samples in the disc of radius
 * diameter / 2 centred on it, a sample at distance d with value v weighing
 * exp(-d^2 / (2 @p sigmaSpace^2)) * exp(-(v - c)^2 / (2 @p sigmaColour^2)),
 * where c is the centre sample, all on the sample values as read. A centre
 * that is not finite (NaN, +inf or -inf) is written unchanged; neighbours
 * that are not finite are skipped, contributing nothing. Pixels beyond the level read
 * as zero. Computed in double in one fixed order, so the result does not
 * depend on the tiling.
 *
 * Its parameters are `diameter` (an integer; 5 when created by name without
 * it), `sigmaColour` and `sigmaSpace` (doubles; 25.0 and 5.0 by default), and
 * Operation::parameters() always holds all three. Creating it by name with the
 * same values gives equal parameters. By name, an unknown key, a value of the
 * wrong type or a value out of range throws InvalidArgumentError naming the
 * key, never the value.
 * @param diameter The window's diameter; odd, from 3 to 255.
 * @param sigmaColour The value-difference scale; finite and at least 1e-6,
 * below which its square may underflow.
 * @param sigmaSpace The distance scale in pixels; finite and at least 1e-6.
 * @return The operation, never null.
 * @throws InvalidArgumentError When @p diameter is even or outside 3 to 255,
 * or a sigma is not finite or below 1e-6; the message names the operation
 * and the parameter, never the value.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * Safe to call concurrently. The operation's apply() may run concurrently on
 * different contexts.
 * @par Requirements
 * UR-PIX-3, UR-QA-2, UR-IMG-11, UR-ALG-6
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS OperationPtr makeBilateralFilter(int32_t diameter, double sigmaColour,
                                                                  double sigmaSpace);

/**
 * @brief The shape of a StructuringElement drawn from its size.
 *
 * Stored in a morphology operation's parameters as its enumerator spelling,
 * `"Rectangle"`, `"Ellipse"` or `"Cross"`; an element built by
 * StructuringElement::custom() is stored as `"Custom"`.
 * @par Requirements
 * UR-PIX-4
 */
enum class StructuringShape : int32_t {
    Rectangle, ///< Every pixel of the width x height box.
    Ellipse,   ///< The ellipse inscribed in the box, drawn as OpenCV's MORPH_ELLIPSE.
    Cross      ///< The centre row and the centre column of the box.
};

/**
 * @brief The neighbourhood a morphology operation takes its minimum or
 * maximum over: a width x height mask, anchored at its centre.
 *
 * Width and height are odd, from 1 to 255. A shape is drawn as OpenCV's
 * `getStructuringElement` draws MORPH_RECT, MORPH_ELLIPSE and MORPH_CROSS.
 * The set pixel in column i and row j of the mask covers the sample at offset
 * (i - width / 2, j - height / 2) from the one being computed.
 *
 * A moved-from element may only be assigned to or destroyed; its width() and
 * height() return 0, and a morphology factory refuses it.
 * @par Thread-safety
 * ThreadCompatible.
 * @par Requirements
 * UR-PIX-4
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS StructuringElement {
public:
    /**
     * @brief Creates an element of a shape drawn in a width x height box.
     *
     * The first argument is the StructuringShape to draw.
     * @param width The box's width; odd, from 1 to 255.
     * @param height The box's height; odd, from 1 to 255.
     * @throws InvalidArgumentError When the shape is not an enumerator or a
     * size is even or outside 1 to 255; the message names the parameter,
     * never the value.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-PIX-4
     */
    StructuringElement(StructuringShape, int32_t width, int32_t height);
    /**
     * @brief Copies the element; the copy shares nothing with the original.
     * @param other The element to copy.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Reads @p other; ThreadCompatible.
     * @par Requirements
     * UR-PIX-4
     */
    StructuringElement(const StructuringElement& other);
    /**
     * @brief Moves the element. The moved-from object may only be assigned to
     * or destroyed; its width() and height() return 0.
     * @param other The element to move from.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-PIX-4
     */
    StructuringElement(StructuringElement&& other) noexcept;
    /**
     * @brief Replaces this element with a copy of another.
     * @param other The element to copy.
     * @return This element.
     * @throws std::bad_alloc When allocation fails; this element is then
     * unchanged.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-PIX-4
     */
    StructuringElement& operator=(const StructuringElement& other);
    /**
     * @brief Replaces this element by moving another. The moved-from object
     * may only be assigned to or destroyed; its width() and height() return 0.
     * @param other The element to move from.
     * @return This element.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-PIX-4
     */
    StructuringElement& operator=(StructuringElement&& other) noexcept;
    /**
     * @brief Destroys the element.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-PIX-4
     */
    ~StructuringElement();

    /**
     * @brief Creates an element from a caller's mask.
     *
     * Copies @p width * @p height bytes, row by row from the top; a non-zero
     * byte is a set pixel. The anchor is the centre.
     * @param mask The bytes; must not be null and must hold at least one
     * non-zero byte.
     * @param width The mask's width; odd, from 1 to 255.
     * @param height The mask's height; odd, from 1 to 255.
     * @return The element.
     * @throws InvalidArgumentError When @p mask is null or has no set pixel,
     * or a size is even or outside 1 to 255; the message names the parameter,
     * never the value.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Safe to call concurrently; reads @p mask.
     * @par Requirements
     * UR-PIX-4
     */
    static StructuringElement custom(const uint8_t* mask, int32_t width, int32_t height);
    /**
     * @brief Returns the element's width.
     * @return An odd number from 1 to 255, or 0 for a moved-from element.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-PIX-4
     */
    int32_t width() const noexcept;
    /**
     * @brief Returns the element's height.
     * @return An odd number from 1 to 255, or 0 for a moved-from element.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-PIX-4
     */
    int32_t height() const noexcept;

private:
    friend struct detail::StructuringElementAccess;
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief Creates `pixel.morph_erode`: PerChannel, halo iterations * (width / 2)
 * left and right and iterations * (height / 2) top and bottom, input `Byte`,
 * `UInt16`, `Int16`, `Float32` or `Float64`, output the input type.
 *
 * Each output sample is the minimum over the element's set pixels, applied
 * @p iterations times. Pixels beyond the level read as zero, so an erosion
 * reaching the level's edge pulls it to zero. In `Float32` and `Float64` a NaN
 * anywhere in the window makes the sample NaN, so the result does not depend
 * on the tiling; between -0 and +0 the first in the window, row by row from
 * the top, is kept.
 *
 * Its parameters are `shape` (`"Rectangle"`, `"Ellipse"`, `"Cross"` or
 * `"Custom"`), `elementWidth` and `elementHeight` (integers), `elementMask` (a
 * double array of 0 and 1, row by row, present only for `"Custom"`) and
 * `iterations` (an integer). Created by name without them they are a 3 x 3
 * `"Rectangle"` and 1 iteration. Creating it by name with the same values gives
 * equal parameters. By name, an unknown key, a value of the wrong type or a
 * value out of range throws InvalidArgumentError naming the key, never the
 * value.
 *
 * The first argument is the StructuringElement, copied.
 * @param iterations How many times the erosion is applied; 1 to 64.
 * @return The operation, never null.
 * @throws InvalidArgumentError When @p iterations is outside 1 to 64 or the
 * element is a moved-from one; the
 * message names the operation and the parameter, never the value.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * Safe to call concurrently. The operation's apply() may run concurrently on
 * different contexts.
 * @par Requirements
 * UR-PIX-4, UR-QA-2, UR-ALG-6
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS OperationPtr makeErode(const StructuringElement&, int32_t iterations);

/**
 * @brief Creates `pixel.morph_dilate`: PerChannel, halo
 * iterations * (width / 2) left and right and iterations * (height / 2) top and
 * bottom, input `Byte`, `UInt16`, `Int16`, `Float32` or `Float64`, output the
 * input type.
 *
 * Each output sample is the maximum over the element's set pixels, applied
 * @p iterations times. Pixels beyond the level read as zero. NaN, -0 and +0
 * are treated as by makeErode().
 *
 * Its parameters are those of makeErode(), with the same defaults by name and
 * the same rules.
 *
 * The first argument is the StructuringElement, copied.
 * @param iterations How many times the dilation is applied; 1 to 64.
 * @return The operation, never null.
 * @throws InvalidArgumentError When @p iterations is outside 1 to 64 or the
 * element is a moved-from one; the
 * message names the operation and the parameter, never the value.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * Safe to call concurrently. The operation's apply() may run concurrently on
 * different contexts.
 * @par Requirements
 * UR-PIX-4, UR-QA-2, UR-ALG-6
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS OperationPtr makeDilate(const StructuringElement&, int32_t iterations);

/**
 * @brief Creates `pixel.morph_open`: PerChannel, halo
 * 2 * iterations * (width / 2) left and right and 2 * iterations * (height / 2)
 * top and bottom, input `Byte`, `UInt16`, `Int16`, `Float32` or `Float64`,
 * output the input type.
 *
 * Erodes @p iterations times, then dilates @p iterations times, with the same
 * element, each step treating NaN, -0 and +0 as makeErode() does. Pixels
 * beyond the level read as zero.
 *
 * Its parameters are those of makeErode(), with the same defaults by name and
 * the same rules.
 *
 * The first argument is the StructuringElement, copied.
 * @param iterations How many times each step is applied; 1 to 64.
 * @return The operation, never null.
 * @throws InvalidArgumentError When @p iterations is outside 1 to 64 or the
 * element is a moved-from one; the
 * message names the operation and the parameter, never the value.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * Safe to call concurrently. The operation's apply() may run concurrently on
 * different contexts.
 * @par Requirements
 * UR-PIX-4, UR-QA-2, UR-ALG-6
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS OperationPtr makeOpen(const StructuringElement&, int32_t iterations);

/**
 * @brief Creates `pixel.morph_close`: PerChannel, halo
 * 2 * iterations * (width / 2) left and right and 2 * iterations * (height / 2)
 * top and bottom, input `Byte`, `UInt16`, `Int16`, `Float32` or `Float64`,
 * output the input type.
 *
 * Dilates @p iterations times, then erodes @p iterations times, with the same
 * element, each step treating NaN, -0 and +0 as makeErode() does. Pixels
 * beyond the level read as zero.
 *
 * Its parameters are those of makeErode(), with the same defaults by name and
 * the same rules.
 *
 * The first argument is the StructuringElement, copied.
 * @param iterations How many times each step is applied; 1 to 64.
 * @return The operation, never null.
 * @throws InvalidArgumentError When @p iterations is outside 1 to 64 or the
 * element is a moved-from one; the
 * message names the operation and the parameter, never the value.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * Safe to call concurrently. The operation's apply() may run concurrently on
 * different contexts.
 * @par Requirements
 * UR-PIX-4, UR-QA-2, UR-ALG-6
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS OperationPtr makeClose(const StructuringElement&, int32_t iterations);

} // namespace slideio::processor
