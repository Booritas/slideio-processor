#include <slideio/processor/storage/geojson_import.hpp>

#include "core/detail/diagnostics.hpp"
#include "storage/detail/errors.hpp"
#include "storage/detail/vocabulary_import.hpp"

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/geojson.hpp>
#include <slideio/processor/core/log.hpp>
#include <slideio/processor/core/measurement.hpp>
#include <slideio/processor/core/object_record.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/object_store.hpp>
#include <slideio/processor/storage/stage_state_reader.hpp>
#include <slideio/processor/storage/store.hpp>
#include <slideio/processor/storage/vocabulary_registry.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <locale>
#include <map>
#include <sstream>
#include <string>

namespace slideio::processor {

namespace {

constexpr const char* kImport = "GeoJsonImporter::import";
constexpr const char* kMeasurementPrefix = "measurements.";

// Tells a geometry error, after which the reader moves on to the next feature, from a parse
// or read error, which ends reading: GeoJsonReader words every geometry error as
// "GeoJsonReader: feature <index> of the file: <reason>".
bool isGeometryError(const InvalidArgumentError& error) {
    static const char kPrefix[] = "GeoJsonReader: feature ";
    return std::strncmp(error.what(), kPrefix, sizeof(kPrefix) - 1) == 0;
}

// Parses `text` as a finite double, independently of the global locale; the whole text
// must be consumed.
bool parseFinite(std::istringstream& in, const char* text, double& out) {
    in.clear();
    in.str(text);
    double value = 0;
    in >> value;
    if (in.fail() || in.peek() != std::char_traits<char>::eof() || !std::isfinite(value)) return false;
    out = value;
    return true;
}

// Walks a file feature by feature, counting skipped features and those the reader rejected
// for their geometry. Features are named by GeoJsonReader::featureIndex(), so the reader's
// messages and the importer's give one feature one index.
class FeatureWalk {
public:
    // `warn` false silences the reader's own warnings, for a pass that only counts.
    FeatureWalk(const char* path, bool warn) : path_(path), reader_(path) { reader_.setWarningsEnabled(warn); }

    // Advances to the next importable feature, checking `token` before each feature;
    // logs and skips features with invalid geometry. False at the end of the file.
    bool next(const CancellationToken& token, const char* reportingFor) {
        for (;;) {
            token.throwIfCancelled();
            try {
                return reader_.next();
            } catch (const InvalidArgumentError& e) {
                if (!isGeometryError(e)) throw;
                if (reportingFor != nullptr) {
                    const std::string lead = detail::withPath(
                        std::string(reportingFor) + ": skipping feature " + std::to_string(reader_.featureIndex()) +
                            " of the file",
                        path_);
                    logMessage(LogLevel::Warning, "%s: %s", lead.c_str(), e.what());
                }
                ++rejected_;
            }
        }
    }
    // Features passed so far, for progress: read, skipped by the reader, or rejected for their
    // geometry. Elements of the feature list that are not objects are not counted.
    int64_t processed() const noexcept { return reader_.featuresRead() + reader_.featuresSkipped() + rejected_; }
    // The index of the current feature, once next() has returned true, as the reader numbers it.
    int64_t index() const noexcept { return reader_.featureIndex(); }
    int64_t skipped() const noexcept { return reader_.featuresSkipped() + rejected_; }
    GeoJsonReader& reader() noexcept { return reader_; }
    // The file's path, as the caller gave it (UTF-8).
    const std::string& path() const noexcept { return path_; }

private:
    std::string path_;
    GeoJsonReader reader_;
    int64_t rejected_ = 0;
};

// "GeoJsonImporter::import: feature <index> of the file (<path>)", the lead of the
// importer's lines about one feature.
std::string featureText(int64_t index, const std::string& path) {
    return detail::withPath(std::string(kImport) + ": feature " + std::to_string(index) + " of the file", path);
}

void report(ProgressObserver* observer, const std::string& stageName, int64_t done, int64_t total) {
    if (observer == nullptr) return;
    const double fraction = total > 0 ? std::min(1.0, static_cast<double>(done) / static_cast<double>(total)) : 1.0;
    observer->onProgress(stageName.c_str(), fraction, done, total);
}

} // namespace

class ImportResult::Impl {
public:
    int64_t objectsImported = 0;
    int32_t labelsRegistered = 0;
    int32_t featuresSkipped = 0;
    ImportPolicy policy = ImportPolicy::Strict;
    bool hasProvenance = false;
    Provenance provenance;
};

ImportResult::ImportResult() noexcept = default;

ImportResult::ImportResult(const ImportResult& other)
    : impl_(other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr) {}

ImportResult::ImportResult(ImportResult&& other) noexcept = default;

ImportResult& ImportResult::operator=(const ImportResult& other) {
    if (this != &other) impl_ = other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr;
    return *this;
}

ImportResult& ImportResult::operator=(ImportResult&& other) noexcept {
    impl_ = std::move(other.impl_);
    return *this;
}

ImportResult::~ImportResult() = default;

int64_t ImportResult::objectsImported() const noexcept { return impl_ ? impl_->objectsImported : 0; }

int32_t ImportResult::labelsRegistered() const noexcept { return impl_ ? impl_->labelsRegistered : 0; }

int32_t ImportResult::featuresSkipped() const noexcept { return impl_ ? impl_->featuresSkipped : 0; }

ImportPolicy ImportResult::policyApplied() const noexcept { return impl_ ? impl_->policy : ImportPolicy::Strict; }

bool ImportResult::hasSourceProvenance() const noexcept { return impl_ && impl_->hasProvenance; }

const Provenance& ImportResult::sourceProvenance() const noexcept {
    static const Provenance kNone;
    return impl_ ? impl_->provenance : kNone;
}

class GeoJsonImporter::Impl {
public:
    Impl(Store& s, LayerId l, StageId st, std::string name)
        : store(s), layer(l), stage(st), stageName(std::move(name)) {}

    // The label `classification` resolves to, through `labels` first. Under Permissive an
    // unresolved one is registered (counted in `registered`); under Strict it throws.
    // `path` is the file's, for the error.
    LabelId resolve(const char* classification, int64_t index, ImportPolicy policy,
                    std::map<std::string, LabelId>& labels, int32_t& registered, const std::string& path) {
        const auto cached = labels.find(classification);
        if (cached != labels.end()) return cached->second;
        LabelId label;
        if (!store.vocabulary().find(classification, label)) {
            if (policy == ImportPolicy::Strict) throwUnknownLabel(classification, index, path);
            bool added = false;
            label = detail::registerImportedLabel(detail::storeImplOf(store), classification, added);
            if (added) ++registered;
        }
        labels.emplace(classification, label);
        return label;
    }

    [[noreturn]] static void throwUnknownLabel(const char* classification, int64_t index, const std::string& path) {
        detail::throwInvalid(kImport, detail::withPath("feature " + std::to_string(index) + " of the file", path) +
                                          " has the label '" + classification +
                                          "', which the vocabulary does not hold (import policy Strict)");
    }

    Store& store;
    LayerId layer;
    StageId stage;
    std::string stageName;
};

GeoJsonImporter::GeoJsonImporter(Store& store, LayerId targetLayer, StageId assertingStage) {
    constexpr const char* kOperation = "GeoJsonImporter";
    if (store.isReadOnly()) {
        const std::string message = detail::withPath(std::string(kOperation) + ": the store is read-only", store.path());
        throw IoError(message.c_str());
    }
    const LayerRegistry& layers = store.layers();
    try {
        (void)layers.kind(targetLayer);  // IoError on a closed store passes through
    } catch (const InvalidArgumentError&) {
        detail::throwInvalid(kOperation, "there is no layer " + std::to_string(targetLayer.value()));
    }
    if (targetLayer == layers.pixelLayer()) detail::throwInvalid(kOperation, "the target layer is the pixel layer");
    if (layers.isDropped(targetLayer)) {
        detail::throwInvalid(kOperation, std::string("the target layer '") + layers.name(targetLayer) + "' is dropped");
    }
    std::string stageName;
    try {
        stageName = store.stageStates().name(assertingStage);
    } catch (const InvalidArgumentError&) {
        detail::throwUnknownStage(kOperation, assertingStage);
    }
    impl_ = std::make_unique<Impl>(store, targetLayer, assertingStage, std::move(stageName));
}

GeoJsonImporter::~GeoJsonImporter() = default;

ImportResult GeoJsonImporter::import(const char* path) { return import(path, nullptr, CancellationToken()); }

ImportResult GeoJsonImporter::import(const char* path, ProgressObserver* observer, const CancellationToken& token) {
    if (path == nullptr) detail::throwInvalid(kImport, "path must not be null");
    ImportResult result;
    result.impl_ = std::make_unique<ImportResult::Impl>();
    ImportResult::Impl& out = *result.impl_;
    out.policy = impl_->store.vocabulary().importPolicy();
    std::map<std::string, LabelId> labels;  // classification as written -> label

    // Pre-pass: count the features; under Strict, resolve every label before writing.
    int64_t total = 0;
    {
        FeatureWalk walk(path, false);
        while (walk.next(token, nullptr)) {
            const char* classification = walk.reader().classification();
            if (out.policy == ImportPolicy::Strict && *classification != '\0') {
                impl_->resolve(classification, walk.index(), out.policy, labels, out.labelsRegistered, walk.path());
            }
        }
        total = walk.processed();
        out.hasProvenance = walk.reader().provenance(out.provenance);
    }

    // Import pass.
    ObjectWriterPtr writer = impl_->store.objects().writerForStage(impl_->stage);
    FeatureWalk walk(path, true);
    std::istringstream number;
    number.imbue(std::locale::classic());
    int64_t pending = 0;    // emitted since the last commit
    int64_t reported = -1;  // features done at the last progress report
    ObjectRecord record;
    for (;;) {
        bool more = false;
        try {
            more = walk.next(token, kImport);
        } catch (const CancelledError&) {
            writer->flush();
            throw;
        }
        if (!more) break;
        GeoJsonReader& reader = walk.reader();
        const int64_t index = walk.index();
        record.reset();
        record.setLayer(impl_->layer);
        record.setGeometry(reader.geometry());
        if (*reader.classification() != '\0') {
            record.setLabel(
                impl_->resolve(reader.classification(), index, out.policy, labels, out.labelsRegistered, walk.path()));
        }
        const size_t prefixLength = std::strlen(kMeasurementPrefix);
        for (int32_t p = 0; p < reader.propertyCount(); ++p) {
            const char* name = reader.propertyName(p);
            if (std::strncmp(name, kMeasurementPrefix, prefixLength) != 0) continue;
            const char* measurement = name + prefixLength;
            double value = 0;
            if (*measurement == '\0') {
                logMessage(LogLevel::Warning, "%s: a measurement has an empty name; skipped",
                           featureText(index, walk.path()).c_str());
            } else if (!parseFinite(number, reader.propertyValue(p), value)) {
                logMessage(LogLevel::Warning, "%s: measurement '%s' is not a finite number; skipped",
                           featureText(index, walk.path()).c_str(), measurement);
            } else {
                record.measurements().set(measurement, value, MeasurementUnit::Dimensionless);
            }
        }
        writer->emit(record);
        if (++pending == writer->batchSizeObjects()) {  // emit() committed the batch
            pending = 0;
            reported = walk.processed();
            report(observer, impl_->stageName, reported, total);
        }
    }
    writer->flush();
    out.objectsImported = writer->emittedCount();
    out.featuresSkipped = static_cast<int32_t>(
        std::min<int64_t>(walk.skipped(), std::numeric_limits<int32_t>::max()));
    // The end, unless the last batch already reported it.
    if (walk.processed() != reported) report(observer, impl_->stageName, walk.processed(), total);
    return result;
}

Polygon importRoiPolygon(const char* path) {
    constexpr const char* kOperation = "importRoiPolygon";
    if (path == nullptr) detail::throwInvalid(kOperation, "path must not be null");
    GeoJsonReader reader(path);
    if (!reader.next()) detail::throwInvalid(kOperation, detail::withPath("the file holds no polygon", path));
    return reader.geometry();
}

} // namespace slideio::processor
