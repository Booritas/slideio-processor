// Consumer of slideio-processor-data alone: creates a store, writes its provenance,
// reopens it read-only and checks what it reads back. Exit 0 on success, 1 on a
// mismatch, 2 on an exception.
#include <slideio/processor/config/configuration.hpp>
#include <slideio/processor/core/provenance.hpp>
#include <slideio/processor/storage/store.hpp>
#include <slideio/processor/storage/types.hpp>

#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>

using slideio::processor::Configuration;
using slideio::processor::OpenMode;
using slideio::processor::Provenance;
using slideio::processor::Store;

int main() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() /
        ("slideio_data_only_consumer_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    const std::string path = (dir / "consumer.store").string();

    int rc = 0;
    try {
        {
            auto rw = Store::open(path.c_str(), OpenMode::CreateNew, Configuration());
            Provenance p = rw->provenance();
            p.setSlideId("/slides/a.svs");
            p.setSceneIndex(2);
            rw->setProvenance(p);
            rw->close();
        }
        {
            auto ro = Store::open(path.c_str(), OpenMode::ReadOnly, Configuration());
            const Provenance p = ro->provenance();
            if (std::string(p.slideId()) != "/slides/a.svs" || p.sceneIndex() != 2) {
                std::fprintf(stderr, "provenance mismatch\n");
                rc = 1;
            } else if (ro->objects().totalObjectCount() != 0) {
                std::fprintf(stderr, "store is not empty\n");
                rc = 1;
            }
            ro->close();
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "exception: %s\n", e.what());
        rc = 2;
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
    if (rc == 0) std::printf("data-only consumer ok\n");
    return rc;
}
