// Requirements: UR-EXE-5
#pragma once

// Helpers for the text of error messages, error causes and log lines. A diagnostic about a
// specific file or directory names it by its path (the rule of 2026-11-23, which revoked "no
// paths in diagnostics": the software runs in a secure environment, and a failure should be
// diagnosable without reproducing it). The library's own messages keep their role wording
// ("cannot open the store file") and append the path with withPath; third-party text
// (std::filesystem, SlideIO, libtiff, SQLite) reaches causes and logs unaltered, paths
// included. Slide file names may carry patient identifiers, so logs and errors can hold PHI:
// access to and retention of them fall under the HIPAA Security Rule and GDPR.
// Header-only, so the data and the engine library can each use it.

#include <exception>
#include <string>

namespace slideio::processor::detail {

// The text of `e` for use as an error cause or in a log line: e.what(), unaltered, for every
// exception (a std::filesystem::filesystem_error's what() names its paths).
inline std::string diagnosticText(const std::exception& e) { return e.what(); }

// `message`, a role-worded diagnostic, followed by the path of the file or directory it is
// about: "<message> (<path>)". `path` is UTF-8 as the caller has it (a std::filesystem::path
// is given as its u8string()).
inline std::string withPath(const std::string& message, const std::string& path) {
    return message + " (" + path + ")";
}

} // namespace slideio::processor::detail
