// Requirements: UR-PIX-1, UR-PIX-2, UR-IMG-7, UR-ALG-3
#pragma once

/**
 * @file colour.hpp
 * @brief Colour spaces, `pixel.colour_convert`, stain vectors and
 * `pixel.stain_deconvolution`.
 */

#include <slideio/processor/pipeline/export.hpp>
#include <slideio/processor/pipeline/operation.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

/**
 * @brief A colour space `pixel.colour_convert` reads or writes.
 *
 * Stored in an operation's parameters as its enumerator spelling, e.g.
 * `"Rgb"` or `"Lab"`.
 * @par Requirements
 * UR-PIX-1
 */
enum class ColourSpace : int32_t {
    Rgb,  ///< sRGB, three planes on the 0-255 scale.
    Grey, ///< One plane on the 0-255 scale; from RGB it is ITU-R BT.601 luma.
    Hsv,  ///< Hexcone HSV: H in degrees [0, 360), S and V in [0, 1].
    Lab   ///< CIE 1976 L*a*b* with D65 white, L* in [0, 100].
};

/**
 * @brief Creates `pixel.colour_convert`: Joint, requires uniform types, halo
 * none, input Byte or Float32, output Float32.
 *
 * Converts each pixel from @p from to @p to. The output has one plane when
 * @p to is ColourSpace::Grey, else three. Every pair converts through RGB,
 * computed in double precision and written as Float32. The units are:
 * - RGB and grey on the 0-255 scale. Grey from RGB is
 *   0.299 R + 0.587 G + 0.114 B; RGB from grey replicates the value.
 * - HSV is the hexcone model: H in degrees [0, 360), 0 when S is 0; S and V in
 *   [0, 1].
 * - Lab is CIE 1976 L*a*b*: sRGB per IEC 61966-2-1 with its linearisation,
 *   D65 white (0.95047, 1, 1.08883), L* in [0, 100].
 *
 * The output is never clamped: an out-of-gamut Lab or HSV input gives RGB
 * outside 0-255 rather than a saturated value. A pixel with a NaN in any
 * input component is NaN in every output component.
 *
 * The input each source space needs is checked when the operation is applied:
 * - ColourSpace::Rgb: exactly three Byte planes holding scene channels 0, 1
 *   and 2, in that order. The channels' meaning is inferred from this, since a
 *   scene states no colour model. Fewer or more planes throw
 *   ChannelArityError; another type or channel order throws
 *   InvalidArgumentError. The message names the operation, says it needs
 *   channels 0, 1, 2 as RGB, Byte, and lists the channels found.
 * - ColourSpace::Grey: one Byte or Float32 plane.
 * - ColourSpace::Hsv and ColourSpace::Lab: three Float32 planes, as this
 *   operation writes them.
 * Otherwise the operation throws ChannelArityError (plane count) or
 * UnsupportedDataTypeError (type), naming the operation. Within a pipeline
 * the run reports the failure as a TileError naming the operation.
 *
 * Its parameters are `from` (`"Rgb"` when created by name without it) and
 * `to` (`"Grey"` by default), and Operation::parameters() always holds both.
 * Creating it by name through OperationCatalog::create with the same values
 * gives equal parameters. By name, an unknown key, a value of the wrong type
 * or an unknown spelling throws InvalidArgumentError naming the key.
 * @param from The colour space of the input.
 * @param to The colour space of the output; differs from @p from.
 * @return The operation, never null.
 * @throws InvalidArgumentError When @p from equals @p to, or either is not an
 * enumerator; the message names the operation and the parameter, never the
 * value.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * Safe to call concurrently. The operation's apply() may run concurrently on
 * different contexts.
 * @par Requirements
 * UR-PIX-1, UR-IMG-7, UR-ALG-3, UR-IMG-11
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS OperationPtr makeColourConvert(ColourSpace from, ColourSpace to);

/**
 * @brief The absorption vectors of up to three stains, with optional names,
 * that `pixel.stain_deconvolution` separates an RGB image into.
 *
 * Each vector is the optical density a unit amount of the stain adds to the
 * red, green and blue channels. Vectors are stored exactly as given and
 * normalised to unit length where they are used, so getVector() returns what
 * setVector() received. A default-constructed object holds no stains;
 * hematoxylinEosin() and hematoxylinDab() return the built-in pairs of
 * Ruifrok and Johnston (Anal Quant Cytol Histol 23:291-299, 2001).
 * @par Thread-safety
 * ThreadCompatible.
 * @par Requirements
 * UR-PIX-2
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS StainVectors {
public:
    /**
     * @brief Creates an object holding no stains; count() is 0.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-PIX-2
     */
    StainVectors();
    /**
     * @brief Copies the vectors and names; the copy shares nothing with the
     * original.
     * @param other The object to copy.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Reads @p other; ThreadCompatible.
     * @par Requirements
     * UR-PIX-2
     */
    StainVectors(const StainVectors& other);
    /**
     * @brief Moves the vectors and names. The moved-from object holds no
     * stains and is reusable.
     * @param other The object to move from.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-PIX-2
     */
    StainVectors(StainVectors&& other) noexcept;
    /**
     * @brief Replaces this object's vectors and names with a copy of another's.
     * @param other The object to copy.
     * @return This object.
     * @throws std::bad_alloc When allocation fails; this object is then
     * unchanged.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-PIX-2
     */
    StainVectors& operator=(const StainVectors& other);
    /**
     * @brief Replaces this object's vectors and names by moving another's.
     * @param other The object to move from; left holding no stains, reusable.
     * @return This object.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-PIX-2
     */
    StainVectors& operator=(StainVectors&& other) noexcept;
    /**
     * @brief Destroys the object.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-PIX-2
     */
    ~StainVectors();

    /**
     * @brief Returns hematoxylin and eosin: stain 0 "Hematoxylin" (0.650,
     * 0.704, 0.286), stain 1 "Eosin" (0.072, 0.990, 0.105).
     * @return The two stains, named.
     * @throws Nothing. Declared noexcept although it allocates: failure to
     * allocate is fatal and calls std::terminate.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-PIX-2
     */
    static StainVectors hematoxylinEosin() noexcept;
    /**
     * @brief Returns hematoxylin and DAB: stain 0 "Hematoxylin" (0.650, 0.704,
     * 0.286), stain 1 "DAB" (0.268, 0.570, 0.776).
     * @return The two stains, named.
     * @throws Nothing. Declared noexcept although it allocates: failure to
     * allocate is fatal and calls std::terminate.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-PIX-2
     */
    static StainVectors hematoxylinDab() noexcept;

    /**
     * @brief Returns the number of stains, 0 to 3.
     * @return The count.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-PIX-2
     */
    int32_t count() const noexcept;
    /**
     * @brief Sets the vector of stain @p index, or appends a stain when
     * @p index equals count().
     *
     * The components are stored as given. An appended stain's name is "".
     * @param index 0 to count(); at most three stains are held.
     * @param r The optical density in the red channel.
     * @param g The optical density in the green channel.
     * @param b The optical density in the blue channel.
     * @throws InvalidArgumentError When @p index is outside 0 to count(), when
     * it would make a fourth stain, or when a component is negative, NaN or
     * infinite, or all three are zero; the message names the method and never
     * a value. The object is then unchanged.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-PIX-2
     */
    void setVector(int32_t index, double r, double g, double b);
    /**
     * @brief Reads the vector of stain @p index, as it was set.
     * @param index 0 to count() - 1.
     * @param r Receives the red component.
     * @param g Receives the green component.
     * @param b Receives the blue component.
     * @throws InvalidArgumentError When @p index is out of range; the outputs
     * are then untouched.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-PIX-2
     */
    void getVector(int32_t index, double& r, double& g, double& b) const;
    /**
     * @brief Returns the name of stain @p index.
     * @param index 0 to count() - 1.
     * @return The UTF-8 name, "" until set; valid until the next non-const call
     * on this object.
     * @throws InvalidArgumentError When @p index is out of range.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-PIX-2
     */
    const char* stainName(int32_t index) const;
    /**
     * @brief Names stain @p index.
     *
     * The second argument is the UTF-8 name, copied; non-null, may be empty.
     * @param index 0 to count() - 1.
     * @throws InvalidArgumentError When @p index is out of range or the name is
     * null; the object is then unchanged.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-PIX-2
     */
    void setStainName(int32_t index, const char*);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief Creates `pixel.stain_deconvolution`: Joint, requires uniform types,
 * halo none, input Byte RGB, output Float32.
 *
 * Separates an RGB image into the amount of each stain by colour
 * deconvolution (Ruifrok and Johnston, 2001), writing one Float32 plane per
 * stain, in the order the stains are held. For each pixel:
 * 1. The optical density of each channel is OD = -log10(max(I, 1) / 255), so a
 *    sample of 0 reads as 1 and white is no stain.
 * 2. Each stain vector is normalised to unit length and becomes a row of a
 *    3 x 3 matrix M. With two stains the third row is the unit cross product
 *    of the first two; its amount is computed but not written.
 * 3. The stain amounts are OD M^-1, computed in double precision.
 *
 * On images composed from known unit vectors and amounts 0 to 1, the amounts
 * are recovered within 0.01 wherever every channel's intensity is at least 64;
 * darker pixels are limited by the 8-bit quantisation of the input.
 *
 * The input is checked when the operation is applied: exactly three Byte
 * planes holding scene channels 0, 1 and 2, in that order. The channels'
 * meaning is inferred from this, since a scene states no colour model. Fewer
 * or more planes throw ChannelArityError; another type or channel order
 * throws InvalidArgumentError. The message names the operation, says it needs
 * channels 0, 1, 2 as RGB, Byte, and lists the channels found. Within a
 * pipeline the run reports the failure as a TileError naming the operation.
 *
 * Its parameters are `vectors`, a double array of 3 x count() components in
 * stain order, as given, and `stainName0` to `stainName<count - 1>`, strings,
 * "" for an unnamed stain; Operation::parameters() always holds them all.
 * Created by name through OperationCatalog::create, an absent `vectors` gives
 * hematoxylin and eosin, as hematoxylinEosin() does, and an absent name gives
 * that pair's name ("Hematoxylin", "Eosin") when `vectors` is absent too, else
 * "". By name, `vectors` of a length other than 6 or 9 or holding an invalid
 * vector, an unknown key or a value of the wrong type throws
 * InvalidArgumentError naming the key.
 *
 * The argument holds two or three linearly independent stains.
 * @return The operation, never null.
 * @throws InvalidArgumentError When the argument holds fewer than two stains, or
 * when the 3 x 3 matrix of unit rows, completed as above, has a determinant
 * below 1e-6 in magnitude (the vectors are linearly dependent); the message
 * names the operation, never a value.
 * @throws std::bad_alloc When memory is exhausted.
 * @par Thread-safety
 * Safe to call concurrently. The operation's apply() may run concurrently on
 * different contexts.
 * @par Requirements
 * UR-PIX-2, UR-IMG-7, UR-ALG-3, UR-IMG-11
 */
SLIDEIO_PROCESSOR_ENGINE_EXPORTS OperationPtr makeStainDeconvolution(const StainVectors&);

} // namespace slideio::processor
