// Requirements: UR-EXE-5
#pragma once

// Helpers that keep file paths out of error messages, error causes and log lines. Slide file
// names often carry patient identifiers (HIPAA, GDPR), and the bindings' errors and logs can
// reach a language model, so a diagnostic names a file by its role and never by its path.
// Header-only, so the data and the engine library can each use it.

#include <algorithm>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace slideio::processor::detail {

// The text of `e` for use as an error cause or in a log line. A std::filesystem::filesystem_error
// carries its paths in what(), so its cause is the error code's message alone; any other
// exception's what() is used as it is.
inline std::string diagnosticText(const std::exception& e) {
    if (const auto* failure = dynamic_cast<const std::filesystem::filesystem_error*>(&e)) {
        return failure->code().message();
    }
    return e.what();
}

// `text` with every occurrence of `needle` replaced by `replacement`; `text` unchanged when
// `needle` is empty.
inline std::string replaceAll(std::string text, const std::string& needle, const std::string& replacement) {
    if (needle.empty()) return text;
    std::size_t at = 0;
    while ((at = text.find(needle, at)) != std::string::npos) {
        text.replace(at, needle.size(), replacement);
        at += replacement.size();
    }
    return text;
}


// `text` with the slide path, as given and made absolute, each in its native and its
// generic ('/') spelling, and then the slide's bare file name, replaced by "<slide>". SlideIO
// words some of its errors with the path it was given, and the file name may carry a patient
// identifier. A path that cannot be made absolute is replaced as given only.
inline std::string redactSlidePath(std::string text, const std::string& slidePath) {
    if (slidePath.empty()) return text;
    constexpr const char* kPlaceholder = "<slide>";
    std::vector<std::string> spellings;
    const auto add = [&spellings](const std::string& spelling) {
        if (!spelling.empty() && std::find(spellings.begin(), spellings.end(), spelling) == spellings.end()) {
            spellings.push_back(spelling);
        }
    };
    try {
        const std::filesystem::path given = std::filesystem::u8path(slidePath);
        add(slidePath);
        add(given.generic_u8string());
        std::error_code error;
        const std::filesystem::path absolute = std::filesystem::absolute(given, error);
        if (!error) {
            add(absolute.u8string());
            add(absolute.generic_u8string());
        }
        add(given.filename().u8string());
    } catch (const std::exception&) {
        add(slidePath);
    }
    // Longest first, so a full path goes before the file name inside it.
    std::sort(spellings.begin(), spellings.end(),
              [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    for (const std::string& spelling : spellings) text = replaceAll(std::move(text), spelling, kPlaceholder);
    return text;
}
} // namespace slideio::processor::detail
