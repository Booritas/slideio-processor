#include <slideio/processor/storage/store.hpp>

#include "core/detail/diagnostics.hpp"
#include "storage/detail/schema.hpp"
#include "storage/detail/store_impl.hpp"

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/log.hpp>

#include <exception>
#include <new>
#include <string>

namespace slideio::processor {

class Store::Impl {
public:
    Impl(const char* path, OpenMode mode, const Configuration& configuration)
        : core(path, mode, configuration), journal(core), layerRegistry(core), rasterRegistry(core), vocabularyRegistry(core), stageReader(core), stageWriter(core), objectStore(core), edgeStore(core, layerRegistry), annotationStore(core, layerRegistry, objectStore) {}

    detail::StoreImpl core;
    EventJournal journal;
    LayerRegistry layerRegistry;
    RasterRegistry rasterRegistry;
    VocabularyRegistry vocabularyRegistry;
    StageStateReader stageReader;
    StageStateWriter stageWriter;
    ObjectStore objectStore;
    EdgeStore edgeStore;
    AnnotationStore annotationStore;
};

Store::Store(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

StorePtr Store::open(const char* path, OpenMode mode, const Configuration& configuration) {
    try {
        return StorePtr(new Store(std::make_unique<Impl>(path, mode, configuration)));
    } catch (const ProcessorError&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        // For example an invalid UTF-8 path refused by std::filesystem.
        const std::string message = std::string("Store::open: cannot open the store");
        ErrorContext context;
        const std::string cause = detail::diagnosticText(e);
        context.cause = cause.c_str();
        throw IoError(message.c_str(), context);
    }
}

Store::~Store() {
    try {
        close();
    } catch (const std::exception& e) {
        logMessage(LogLevel::Error, "Store: closing the store failed: %s", detail::diagnosticText(e).c_str());
    } catch (...) {
        logMessage(LogLevel::Error, "Store: closing the store failed");
    }
}

const char* Store::path() const noexcept { return impl_->core.path().c_str(); }

int32_t Store::formatVersion() const noexcept { return impl_->core.formatVersion(); }

OpenMode Store::mode() const noexcept { return impl_->core.mode(); }

bool Store::isReadOnly() const noexcept { return impl_->core.isReadOnly(); }

bool Store::isOpen() const noexcept { return impl_->core.isOpen(); }

Provenance Store::provenance() const {
    const detail::StoreImpl& core = impl_->core;
    return core.withReadConnection("Store::provenance", [](sqlite3* db) {
        const std::optional<std::string> json = detail::StoreImpl::metaText(db, detail::kMetaProvenance);
        if (!json) {
            const std::string message = "Store::provenance: the store has no provenance record";
            throw InternalError(message.c_str());
        }
        return detail::parseProvenanceJson(*json);
    });
}

void Store::setProvenance(const Provenance& provenance) {
    impl_->core.checkWritable("Store::setProvenance");
    const std::string json = detail::provenanceToJson(provenance);
    impl_->core.withWriteTransaction("Store::setProvenance", [&json](sqlite3* db) {
        detail::StoreImpl::setMetaText(db, detail::kMetaProvenance, json);
    });
}

EventJournal& Store::events() noexcept { return impl_->journal; }

LayerRegistry& Store::layers() noexcept { return impl_->layerRegistry; }

const LayerRegistry& Store::layers() const noexcept { return impl_->layerRegistry; }
RasterRegistry& Store::rasters() noexcept { return impl_->rasterRegistry; }
const RasterRegistry& Store::rasters() const noexcept { return impl_->rasterRegistry; }

VocabularyRegistry& Store::vocabulary() noexcept { return impl_->vocabularyRegistry; }

const VocabularyRegistry& Store::vocabulary() const noexcept { return impl_->vocabularyRegistry; }

ObjectStore& Store::objects() noexcept { return impl_->objectStore; }

EdgeStore& Store::edges() noexcept { return impl_->edgeStore; }

AnnotationStore& Store::annotations() noexcept { return impl_->annotationStore; }

StageStateReader& Store::stageStates() noexcept { return impl_->stageReader; }

StageStateWriter& Store::stageStateWriter() noexcept { return impl_->stageWriter; }

void Store::close() { impl_->core.close(); }

detail::StoreImpl& detail::storeImplOf(Store& store) noexcept { return store.impl_->core; }

#if defined(SLIDEIO_PROCESSOR_TEST_SEAMS)
void detail::trackForCloseTesting(Store& store, const std::shared_ptr<GateClient>& client) {
    store.impl_->core.gate()->track(client);
}
#endif

} // namespace slideio::processor
