// Requirements: UR-PIX-1, UR-IMG-11
#include "detail/colour_math.hpp"

#include <cmath>

namespace slideio::processor::detail {

namespace {

// The sRGB (IEC 61966-2-1) linear RGB to CIE XYZ matrix for D65, in the
// seven-digit form whose rows sum to the white point below.
constexpr double kRgbToXyz[3][3] = {{0.4124564, 0.3575761, 0.1804375},
                                    {0.2126729, 0.7151522, 0.0721750},
                                    {0.0193339, 0.1191920, 0.9503041}};

// D65 reference white.
constexpr double kWhite[3] = {0.95047, 1.0, 1.08883};

// CIE constants: delta = 6/29; f(t) is linear below delta^3.
constexpr double kDelta = 6.0 / 29.0;
constexpr double kDelta3 = kDelta * kDelta * kDelta;
constexpr double kThreeDelta2 = 3.0 * kDelta * kDelta;

struct Matrix3 {
    double m[3][3];
};

// The exact inverse of kRgbToXyz in double precision, so Lab round trips are
// limited only by rounding, not by a rounded published inverse.
Matrix3 invert(const double (&a)[3][3]) {
    const double c00 = a[1][1] * a[2][2] - a[1][2] * a[2][1];
    const double c01 = a[1][2] * a[2][0] - a[1][0] * a[2][2];
    const double c02 = a[1][0] * a[2][1] - a[1][1] * a[2][0];
    const double det = a[0][0] * c00 + a[0][1] * c01 + a[0][2] * c02;
    Matrix3 inv{};
    inv.m[0][0] = c00 / det;
    inv.m[1][0] = c01 / det;
    inv.m[2][0] = c02 / det;
    inv.m[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / det;
    inv.m[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / det;
    inv.m[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / det;
    inv.m[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det;
    inv.m[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det;
    inv.m[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det;
    return inv;
}

const Matrix3& xyzToRgb() {
    static const Matrix3 inverse = invert(kRgbToXyz);
    return inverse;
}

// sRGB companding, on [0, 1] nominally; the formulas extend beyond it.
double linearise(double c) {
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double compand(double l) {
    return l <= 0.0031308 ? 12.92 * l : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
}

double labF(double t) {
    return t > kDelta3 ? std::cbrt(t) : t / kThreeDelta2 + 4.0 / 29.0;
}

double labFInverse(double f) {
    return f > kDelta ? f * f * f : kThreeDelta2 * (f - 4.0 / 29.0);
}

} // namespace

double rgbToGrey(const Triple& rgb) noexcept {
    return 0.299 * rgb.c0 + 0.587 * rgb.c1 + 0.114 * rgb.c2;
}

Triple rgbToHsv(const Triple& rgb) noexcept {
    const double r = rgb.c0;
    const double g = rgb.c1;
    const double b = rgb.c2;
    const double max = std::fmax(r, std::fmax(g, b));
    const double min = std::fmin(r, std::fmin(g, b));
    const double delta = max - min;
    const double v = max / 255.0;
    const double s = max > 0.0 ? delta / max : 0.0;
    double h = 0.0;
    // H is 0 whenever S is 0, also out of gamut where every component is at or
    // below zero and the components still differ.
    if (delta > 0.0 && s > 0.0) {
        if (max == r) {
            h = 60.0 * ((g - b) / delta);
        } else if (max == g) {
            h = 60.0 * ((b - r) / delta + 2.0);
        } else {
            h = 60.0 * ((r - g) / delta + 4.0);
        }
        if (h < 0.0) {
            h += 360.0;
        }
        // A hue just below 360 may round to 360 in Float32; it is 0 then.
        if (h >= 360.0 || static_cast<float>(h) >= 360.0f) {
            h = 0.0;
        }
    }
    return {h, s, v};
}

Triple hsvToRgb(const Triple& hsv) noexcept {
    double h = std::fmod(hsv.c0, 360.0);
    if (h < 0.0) {
        h += 360.0;
    }
    const double s = hsv.c1;
    const double v = hsv.c2;
    const double c = v * s;
    const double sector = h / 60.0;
    const double x = c * (1.0 - std::fabs(std::fmod(sector, 2.0) - 1.0));
    const double m = v - c;
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    if (sector < 1.0) {
        r = c;
        g = x;
    } else if (sector < 2.0) {
        r = x;
        g = c;
    } else if (sector < 3.0) {
        g = c;
        b = x;
    } else if (sector < 4.0) {
        g = x;
        b = c;
    } else if (sector < 5.0) {
        r = x;
        b = c;
    } else {
        r = c;
        b = x;
    }
    return {(r + m) * 255.0, (g + m) * 255.0, (b + m) * 255.0};
}

Triple rgbToLab(const Triple& rgb) noexcept {
    const double linear[3] = {linearise(rgb.c0 / 255.0), linearise(rgb.c1 / 255.0), linearise(rgb.c2 / 255.0)};
    double f[3];
    for (int i = 0; i < 3; ++i) {
        const double t =
            (kRgbToXyz[i][0] * linear[0] + kRgbToXyz[i][1] * linear[1] + kRgbToXyz[i][2] * linear[2]) / kWhite[i];
        f[i] = labF(t);
    }
    return {116.0 * f[1] - 16.0, 500.0 * (f[0] - f[1]), 200.0 * (f[1] - f[2])};
}

Triple labToRgb(const Triple& lab) noexcept {
    const double fy = (lab.c0 + 16.0) / 116.0;
    const double fx = fy + lab.c1 / 500.0;
    const double fz = fy - lab.c2 / 200.0;
    const double xyz[3] = {labFInverse(fx) * kWhite[0], labFInverse(fy) * kWhite[1], labFInverse(fz) * kWhite[2]};
    const Matrix3& inv = xyzToRgb();
    double out[3];
    for (int i = 0; i < 3; ++i) {
        out[i] = compand(inv.m[i][0] * xyz[0] + inv.m[i][1] * xyz[1] + inv.m[i][2] * xyz[2]) * 255.0;
    }
    return {out[0], out[1], out[2]};
}

} // namespace slideio::processor::detail
