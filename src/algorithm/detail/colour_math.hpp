// Requirements: UR-PIX-1, UR-IMG-11
#pragma once

namespace slideio::processor::detail {

// Pixel conversions between sRGB and the other colour spaces, in double
// precision. RGB and grey are on the 0-255 scale. Nothing is clamped: a value
// outside a space's gamut converts to a value outside the other's range.

/// A pixel of three components, in the order of its colour space's name.
struct Triple {
    double c0;
    double c1;
    double c2;
};

/// ITU-R BT.601 luma: 0.299 R + 0.587 G + 0.114 B.
double rgbToGrey(const Triple& rgb) noexcept;

/// Hexcone HSV: H in degrees [0, 360), 0 when S is 0; S and V in [0, 1]. H is
/// kept below 360 also after rounding to Float32, so it can be written as one.
Triple rgbToHsv(const Triple& rgb) noexcept;

/// The inverse of rgbToHsv. Any H is taken modulo 360.
Triple hsvToRgb(const Triple& hsv) noexcept;

/// CIE 1976 L*a*b* of an sRGB pixel (IEC 61966-2-1 linearisation and matrix),
/// D65 white (0.95047, 1, 1.08883).
Triple rgbToLab(const Triple& rgb) noexcept;

/// The inverse of rgbToLab.
Triple labToRgb(const Triple& lab) noexcept;

} // namespace slideio::processor::detail
