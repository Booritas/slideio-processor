#include "storage/detail/raster_files.hpp"

#include <cstdio>
#include <random>

namespace slideio::processor::detail {

namespace {

constexpr size_t kStemDigits = 32;

} // namespace

std::filesystem::path rasterDirectory(const std::string& storePath) {
    return std::filesystem::u8path(storePath + "-rasters");
}

std::string generateRasterFileStem() {
    std::random_device device;
    std::string stem;
    for (size_t i = 0; i < kStemDigits / 8; ++i) {
        char word[9];
        std::snprintf(word, sizeof(word), "%08x", static_cast<unsigned>(device()));
        stem += word;
    }
    return stem;
}

bool isGeneratedRasterFileName(const std::string& fileName) noexcept {
    if (fileName.size() <= kStemDigits || fileName[kStemDigits] != '.') return false;
    for (size_t i = 0; i < kStemDigits; ++i) {
        const char c = fileName[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    const size_t extension = kStemDigits + 1;
    return fileName.compare(extension, std::string::npos, "tif") == 0 ||
           fileName.compare(extension, std::string::npos, "spool") == 0;
}

} // namespace slideio::processor::detail
