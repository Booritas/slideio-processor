// Requirements: UR-RES-9, UR-RES-11, UR-RES-15
#include "reporting/detail/csv_format.hpp"

// <charconv> defines __cpp_lib_to_chars; without it appendDouble() falls back to a
// classic-locale stream.
#if __has_include(<charconv>)
#include <charconv>
#endif
#include <cmath>
#include <locale>
#include <sstream>

namespace slideio::processor::detail {

bool isRefusedDelimiter(char c) noexcept {
    return c == '"' || c == '\r' || c == '\n' || c == '#';
}

void appendEscapedLabel(std::string& cell, const char* name) {
    for (const char* p = name; *p != '\0'; ++p) {
        if (*p == '\\' || *p == ';') cell += '\\';
        cell += *p;
    }
}

void appendOneLine(std::string& out, const char* text) {
    for (const char* p = text; *p != '\0'; ++p) {
        if (*p == '\r') {
            out += "\\r";
        } else if (*p == '\n') {
            out += "\\n";
        } else {
            out += *p;
        }
    }
}

void appendField(std::string& line, const std::string& field, char delimiter) {
    bool quote = false;
    for (const char c : field) {
        // '#' too: pandas.read_csv(comment='#') cuts a line at an unquoted '#'.
        if (c == delimiter || c == '"' || c == '\r' || c == '\n' || c == '#') {
            quote = true;
            break;
        }
    }
    if (!quote) {
        line += field;
        return;
    }
    line += '"';
    for (const char c : field) {
        if (c == '"') line += '"';
        line += c;
    }
    line += '"';
}

void appendDouble(std::string& out, double value) {
    // Non-finite values are spelled explicitly, the same on every platform, in the
    // forms pandas parses.
    if (std::isnan(value)) {
        out += "nan";
        return;
    }
    if (std::isinf(value)) {
        out += value > 0 ? "inf" : "-inf";
        return;
    }
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
    // The standard defines this conversion as printf's "%.17g" in the C locale.
    char buffer[64];
    const std::to_chars_result r =
        std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general, 17);
    out.append(buffer, r.ptr);
#else
    // A classic-locale stream with precision 17 and no floatfield formats as "%.17g",
    // whatever the global C locale says the decimal point is.
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream.precision(17);
    stream << value;
    out += stream.str();
#endif
}

} // namespace slideio::processor::detail
