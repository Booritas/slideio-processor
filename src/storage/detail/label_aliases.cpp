#include "storage/detail/label_aliases.hpp"

namespace slideio::processor::detail {

namespace {

struct Alias {
    const char* key;        // folded QuPath spelling
    const char* canonical;  // standard vocabulary name
};

constexpr Alias kAliases[] = {
    {"tumor", "tumour"},
    {"artifact", "artefact"},
    {"immune cells", "immune"},
    {"ignore*", "ignore"},
};

} // namespace

std::string foldAscii(const char* name) {
    std::string out = name != nullptr ? name : "";
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

const char* canonicalForAlias(const std::string& folded) noexcept {
    for (const Alias& alias : kAliases) {
        if (folded == alias.key) return alias.canonical;
    }
    return nullptr;
}

} // namespace slideio::processor::detail
