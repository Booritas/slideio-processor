// Test-only: store paths that clean up after themselves, and a fixture holding a fresh store.
#pragma once

#include "common/temp_path.hpp"

#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/storage/store.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

namespace slideio::processor {

// Removes a store file and every file that sits beside it: -wal, -shm and -writer.lock.
inline void removeStoreFiles(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    for (const char* suffix : {"-wal", "-shm", "-writer.lock"}) {
        std::filesystem::path side = path;
        side += suffix;
        std::filesystem::remove(side, ec);
    }
}

// A unique store path whose files are removed when the guard goes out of scope.
class StoreFilesGuard {
public:
    explicit StoreFilesGuard(const char* stem = "store") : path_(uniqueTempPath(stem, ".store")) {}
    StoreFilesGuard(const StoreFilesGuard&) = delete;
    StoreFilesGuard& operator=(const StoreFilesGuard&) = delete;
    ~StoreFilesGuard() { removeStoreFiles(path_); }

    const std::filesystem::path& path() const { return path_; }
    // The path as UTF-8, for Store::open.
    std::string utf8() const { return path_.u8string(); }

private:
    std::filesystem::path path_;
};

// Creates a store (CreateNew) before each test; closes it and removes its files afterwards.
class StoreFixture : public ::testing::Test {
protected:
    void SetUp() override { store_ = Store::open(files_.utf8().c_str(), OpenMode::CreateNew, config_); }
    void TearDown() override {
        if (store_) {
            store_->close();
            store_.reset();
        }
    }

    Store& store() { return *store_; }
    // Closes the fixture's store and reopens the file in `mode`.
    StorePtr reopen(OpenMode mode) {
        store_->close();
        store_ = Store::open(files_.utf8().c_str(), mode, config_);
        return store_;
    }
    std::string pathUtf8() const { return files_.utf8(); }
    const Configuration& config() const { return config_; }

private:
    StoreFilesGuard files_;
    Configuration config_;
    StorePtr store_;
};

} // namespace slideio::processor
