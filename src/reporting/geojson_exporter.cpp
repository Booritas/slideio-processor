// Requirements: UR-RES-8, UR-RES-11, UR-RES-15, UR-RES-18, UR-SEG-6, UR-QA-3
#include <slideio/processor/reporting/geojson_exporter.hpp>

#include "reporting/detail/columns.hpp"
#include "reporting/detail/export_driver.hpp"

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/geojson.hpp>
#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/core/measurement.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/core/provenance.hpp>
#include <slideio/processor/storage/annotation_store.hpp>
#include <slideio/processor/storage/edge_store.hpp>
#include <slideio/processor/storage/store.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace slideio::processor {
namespace {

constexpr const char* kWho = "GeoJsonExporter::write";

[[noreturn]] void throwWriteFailed() {
    throw IoError((std::string(kWho) + ": writing the output file failed").c_str());
}

// Streams features through GeoJsonWriter into the partial file the driver names. The
// writer's own IoError messages are replaced by ones that name the output file by its role.
class GeoJsonSink : public detail::ExportSink {
public:
    GeoJsonSink(Store& store, const detail::ResolvedColumns& resolved, LabelPolicy labelPolicy,
                bool parent, const std::string* slideId)
        : store_(store), labelPolicy_(labelPolicy), parent_(parent), slideId_(slideId) {
        for (const detail::ResolvedColumn& column : resolved.columns) {
            if (column.kind == detail::ColumnKind::Measurement) {
                measurements_.push_back(&column);
            } else if (column.kind == detail::ColumnKind::AssertedMeasurement) {
                measurements_.push_back(&column);
                // A stage the store lacks gives no values, so needs no read.
                if (column.stage.isValid()) readAsserted_ = true;
            }
        }
    }

    void begin(const std::filesystem::path& partialPath) override {
        partial_ = partialPath;
        try {
            writer_.emplace(partialPath.u8string().c_str());
        } catch (const IoError&) {
            throw IoError((std::string(kWho) + ": cannot create the output file").c_str());
        }
        try {
            Provenance provenance = store_.provenance();
            if (slideId_ != nullptr) provenance.setSlideId(slideId_->c_str());
            writer_->begin(provenance);
        } catch (const IoError&) {
            throwWriteFailed();
        }
    }

    void writeObject(const ObjectRecord& record, detail::NameCache& names) override {
        const ObjectId id = record.id();
        copyMeasurements(record);
        collectLabels(record, names);

        ObjectId parent;
        const bool hasParent = parent_ && store_.edges().parentOf(id, parent);

        try {
            writer_->writeFeature(feature_, names.layer(record.layer()).c_str(),
                                  labelNames_.data(), labelLocal_.get(),
                                  static_cast<int32_t>(labelNames_.size()),
                                  hasParent ? &parent : nullptr);
        } catch (const IoError&) {
            throwWriteFailed();
        }
    }

    int64_t finish() override {
        try {
            writer_->end();
        } catch (const IoError&) {
            throwWriteFailed();
        }
        // The finished partial file's size, taken before the driver renames it.
        std::error_code ec;
        const std::uintmax_t size = std::filesystem::file_size(partial_, ec);
        if (ec) throwWriteFailed();
        return static_cast<int64_t>(size);
    }

    void close() noexcept override { writer_.reset(); }

private:
    // feature_ becomes the record with only the selected measurements, in column order,
    // under the column names and with their source units; non-finite values are left out.
    void copyMeasurements(const ObjectRecord& record) {
        feature_ = record;
        MeasurementSet& out = feature_.measurements();
        out.clear();
        if (readAsserted_) detail::readAssertedValues(store_, record.id(), asserted_);
        for (const detail::ResolvedColumn* column : measurements_) {
            Measurement value;
            const Measurement* found = nullptr;
            if (column->kind == detail::ColumnKind::Measurement) {
                if (record.measurements().find(column->measurement.c_str(), value))
                    found = &value;
            } else if (readAsserted_) {
                found = detail::findAsserted(asserted_, *column);
            }
            if (found != nullptr && std::isfinite(found->value()))
                out.set(column->name.c_str(), found->value(), found->unit());
        }
    }

    // labelNames_ and labelLocal_ become the labels the policy passes; the names point
    // into the cache, which outlives the export.
    void collectLabels(const ObjectRecord& record, detail::NameCache& names) {
        labelNames_.clear();
        std::vector<LabelId>& ids = labelIds_;
        ids.clear();
        if (labelPolicy_ == LabelPolicy::ProducerOnly) {
            if (record.hasLabel()) ids.push_back(record.label());
        } else {
            AnnotationStore& annotations = store_.annotations();
            const int32_t count = annotations.labelCount(record.id());
            for (int32_t i = 0; i < count; ++i) ids.push_back(annotations.labelAt(record.id(), i));
        }
        if (ids.size() > labelCapacity_) {
            labelLocal_ = std::make_unique<bool[]>(ids.size());
            labelCapacity_ = ids.size();
        }
        for (size_t i = 0; i < ids.size(); ++i) {
            const detail::NameCache::LabelInfo& info = names.label(ids[i]);
            labelNames_.push_back(info.name.c_str());
            labelLocal_[i] = info.isWorkspaceLocal;
        }
    }

    Store& store_;
    LabelPolicy labelPolicy_;
    bool parent_;
    const std::string* slideId_; // null: the store's slide identifier
    std::vector<const detail::ResolvedColumn*> measurements_;
    bool readAsserted_ = false;
    std::filesystem::path partial_;
    std::optional<GeoJsonWriter> writer_;
    ObjectRecord feature_;
    detail::AssertedValues asserted_;
    std::vector<LabelId> labelIds_;
    std::vector<const char*> labelNames_;
    std::unique_ptr<bool[]> labelLocal_;
    size_t labelCapacity_ = 0;
};

} // namespace

class GeoJsonExporter::Impl {
public:
    std::string path;
    ColumnSchema schema = ColumnSchema().allMeasurements().units(UnitPolicy::BothColumns);
    std::optional<std::string> slideId; // empty: the store's slide identifier
};

GeoJsonExporter::GeoJsonExporter(const char* path) {
    if (path == nullptr || *path == '\0')
        throw InvalidArgumentError("GeoJsonExporter: path must not be null or empty");
    auto impl = std::make_unique<Impl>();
    impl->path = path;
    impl_ = std::move(impl);
}

GeoJsonExporter::~GeoJsonExporter() = default;

GeoJsonExporter& GeoJsonExporter::slideId(const char* id) {
    if (id == nullptr) {
        impl_->slideId.reset();
    } else {
        impl_->slideId = std::string(id);
    }
    return *this;
}

GeoJsonExporter& GeoJsonExporter::schema(const ColumnSchema& schema) {
    impl_->schema = schema;
    return *this;
}

ExportResult GeoJsonExporter::write(Store& store, const ObjectQuery& query) {
    return write(store, query, nullptr, CancellationToken());
}

ExportResult GeoJsonExporter::write(Store& store, const ObjectQuery& query,
                                    ProgressObserver* observer, const CancellationToken& token) {
    const detail::ResolvedColumns resolved =
        detail::resolveColumns(impl_->schema, store, detail::ColumnLayout::MeasurementsOnly);
    detail::requireDistinctNames(resolved, kWho);
    const ColumnSchema::Impl& settings = detail::ColumnSchemaAccess::impl(impl_->schema);
    GeoJsonSink sink(store, resolved, settings.labelPolicy, settings.parent,
                     impl_->slideId ? &*impl_->slideId : nullptr);
    return detail::runExport(store, query, std::filesystem::u8path(impl_->path), kWho, sink,
                             observer, token);
}

} // namespace slideio::processor
