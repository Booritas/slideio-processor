// Requirements: UR-PIX-1, UR-IMG-7, UR-ALG-3
#pragma once

/**
 * @file colour.hpp
 * @brief Colour spaces and `pixel.colour_convert`.
 */

#include <slideio/processor/pipeline/export.hpp>
#include <slideio/processor/pipeline/operation.hpp>

#include <cstdint>

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
 * outside 0-255 rather than a saturated value.
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

} // namespace slideio::processor
