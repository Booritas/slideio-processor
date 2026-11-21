// Requirements: UR-RES-12
// Test-only: the store format fixtures (ruling S53) shared by the v1 and v2 tests. A schema
// dump compared with a checked-in snapshot, and the golden content: written into a new store
// by the generator, checked through every public read by the tests.
#pragma once

#include <slideio/processor/storage/store.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

namespace slideio::processor::golden {

// Where the checked-in fixtures live, and where a failing snapshot test writes its dump.
std::filesystem::path dataDir();
std::filesystem::path buildDir();

// `store` with `suffix` appended to its file name ("-rasters", "-wal").
std::filesystem::path sidePath(const std::filesystem::path& store, const char* suffix);

// The S53 dump of the store at `path`: `type|name|tbl_name|sql` of every sqlite_master row,
// ordered by type and name, then the sorted meta keys. Lines end in "\n" alone.
std::string schemaDump(const std::filesystem::path& path);

// The schema dump of a fresh CreateNew store.
std::string freshSchemaDump();

// Compares `actual` with the snapshot `snapshotFile` in dataDir(), ignoring carriage returns.
// On a difference, writes `actual` to `actualFile` in buildDir() and fails with `frozenMessage`
// and both paths.
void expectSchemaMatchesSnapshot(const std::string& actual, const char* snapshotFile, const char* actualFile,
                                 const char* frozenMessage);

// Copies a store file and its -rasters directory to `to`.
void copyStore(const std::filesystem::path& from, const std::filesystem::path& to);

// A directory in the temp directory, removed with its contents when the guard goes out of scope.
class TempDirGuard {
public:
    explicit TempDirGuard(const char* stem);
    TempDirGuard(const TempDirGuard&) = delete;
    TempDirGuard& operator=(const TempDirGuard&) = delete;
    ~TempDirGuard();

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

// Which golden content a store holds. V1 is what the v1 golden store was written with. V2 adds
// one stage, "review", declared after the others and marked stale.
enum class GoldenContent { V1, V2 };

// Writes the V2 golden content into a new store at `path` and closes it. The library writes
// only the current format, so there is no V1 writer: the v1 golden store is checked in.
void writeGoldenStore(const std::filesystem::path& path);

// Checks `content` through every public read, and that the store reports `formatVersion`.
void expectGoldenContent(Store& store, int32_t formatVersion, GoldenContent content);

}  // namespace slideio::processor::golden
