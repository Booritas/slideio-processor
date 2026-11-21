// Requirements: UR-RES-9, UR-RES-11, UR-RES-15
#pragma once

// Internal: the text rules of a CSV export. Not installed.

#include <string>

namespace slideio::processor::detail {

/** True when c may not delimit fields: '"', CR, LF or '#'. */
bool isRefusedDelimiter(char c) noexcept;

/** Appends a label name to a ';'-joined list cell, writing '\' as "\\" and ';' as "\;". */
void appendEscapedLabel(std::string& cell, const char* name);

/** Appends text with CR and LF written as the two characters "\r" and "\n". */
void appendOneLine(std::string& out, const char* text);

/**
 * Appends one field as RFC 4180 does: quoted, with '"' doubled, when it holds the
 * delimiter, '"', CR or LF; as it is otherwise.
 */
void appendField(std::string& line, const std::string& field, char delimiter);

/** Appends a double as printf's "%.17g" would in the C locale. */
void appendDouble(std::string& out, double value);

} // namespace slideio::processor::detail
