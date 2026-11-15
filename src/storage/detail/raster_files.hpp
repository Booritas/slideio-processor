// Requirements: UR-RES-7, UR-STG-9
#pragma once

// Where raster files live and what they are called (rulings S44 and S51). The directory
// is the store path with "-rasters" appended, beside the store as its -wal file is. A
// raster's file is "<stem>.tif" and its spool "<stem>.spool", where the stem is 32
// random lowercase hex digits. A row stores the file name only, never a path, so a
// moved or copied workspace still resolves.

#include <filesystem>
#include <string>

namespace slideio::processor::detail {

// The raster directory of the store at `storePath` (UTF-8): that path with "-rasters"
// appended. Nothing is created. Throws std::bad_alloc.
std::filesystem::path rasterDirectory(const std::string& storePath);

// 32 random lowercase hex digits from std::random_device, as for meta.store_uuid.
// Throws std::bad_alloc, or what std::random_device throws.
std::string generateRasterFileStem();

// Whether `fileName` (a bare name, no directory) is one the library generates: it
// matches ^[0-9a-f]{32}\.(tif|spool)$. The sweep touches no other name.
bool isGeneratedRasterFileName(const std::string& fileName) noexcept;

} // namespace slideio::processor::detail
