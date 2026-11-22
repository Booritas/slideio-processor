// Requirements: UR-API-2, UR-STG-4
#include <slideio/processor/run/export_spec.hpp>

#include <slideio/processor/core/error.hpp>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace slideio::processor {

class ExportSpec::Impl {
public:
    ExportKind kind = ExportKind::Csv;
    std::string file;
    std::string layer;
    std::string raster;
    bool hasLayer = false;
    bool hasRaster = false;
    std::vector<std::string> measurements;
};

namespace {

const char* kindName(ExportKind kind) {
    switch (kind) {
        case ExportKind::Csv:
            return "csv";
        case ExportKind::GeoJson:
            return "geojson";
        case ExportKind::LabelMask:
            return "labelMask";
        case ExportKind::RasterTiff:
            return "rasterTiff";
    }
    return "export";
}

[[noreturn]] void fail(ExportKind kind, const std::string& what) {
    throw InvalidArgumentError(("ExportSpec: the " + std::string(kindName(kind)) + " export " + what).c_str());
}

void requireName(ExportKind kind, const char* value, const char* member) {
    if (value == nullptr || value[0] == '\0') {
        fail(kind, std::string("has a null or empty '") + member + "'");
    }
}

// A plain file name: not empty, not "." or "..", and without a path separator or a drive colon.
void requirePlainFileName(ExportKind kind, const char* file) {
    requireName(kind, file, "file");
    if (std::strcmp(file, ".") == 0 || std::strcmp(file, "..") == 0 || std::strpbrk(file, "/\\:") != nullptr) {
        fail(kind, "has a 'file' that is not a plain file name");
    }
}

} // namespace

ExportSpec ExportSpec::make(ExportKind kind, const char* file, const char* target) {
    const bool onRaster = kind == ExportKind::RasterTiff;
    requirePlainFileName(kind, file);
    requireName(kind, target, onRaster ? "raster" : "layer");
    auto impl = std::make_unique<ExportSpec::Impl>();
    impl->kind = kind;
    impl->file = file;
    if (onRaster) {
        impl->raster = target;
        impl->hasRaster = true;
    } else {
        impl->layer = target;
        impl->hasLayer = true;
    }
    return ExportSpec(std::move(impl));
}

ExportSpec::ExportSpec(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

ExportSpec ExportSpec::csv(const char* file, const char* layer) { return make(ExportKind::Csv, file, layer); }

ExportSpec ExportSpec::geoJson(const char* file, const char* layer) { return make(ExportKind::GeoJson, file, layer); }

ExportSpec ExportSpec::labelMask(const char* file, const char* layer) {
    return make(ExportKind::LabelMask, file, layer);
}

ExportSpec ExportSpec::rasterTiff(const char* file, const char* raster) {
    return make(ExportKind::RasterTiff, file, raster);
}

ExportSpec::ExportSpec(const ExportSpec& other) : impl_(std::make_unique<Impl>(*other.impl_)) {}

ExportSpec::ExportSpec(ExportSpec&& other) noexcept = default;

ExportSpec& ExportSpec::operator=(const ExportSpec& other) {
    if (this != &other) {
        impl_ = std::make_unique<Impl>(*other.impl_);
    }
    return *this;
}

ExportSpec& ExportSpec::operator=(ExportSpec&& other) noexcept = default;

ExportSpec::~ExportSpec() = default;

ExportSpec& ExportSpec::measurement(const char* name) {
    if (impl_->kind != ExportKind::Csv) {
        fail(impl_->kind, "does not take measurements");
    }
    requireName(impl_->kind, name, "measurements");
    impl_->measurements.emplace_back(name);
    return *this;
}

ExportKind ExportSpec::kind() const noexcept { return impl_->kind; }

const char* ExportSpec::file() const noexcept { return impl_->file.c_str(); }

const char* ExportSpec::layer() const noexcept { return impl_->hasLayer ? impl_->layer.c_str() : nullptr; }

const char* ExportSpec::raster() const noexcept { return impl_->hasRaster ? impl_->raster.c_str() : nullptr; }

int32_t ExportSpec::measurementCount() const noexcept { return static_cast<int32_t>(impl_->measurements.size()); }

const char* ExportSpec::measurementAt(int32_t index) const {
    if (index < 0 || static_cast<size_t>(index) >= impl_->measurements.size()) {
        fail(impl_->kind, "has a measurement index out of range");
    }
    return impl_->measurements[static_cast<size_t>(index)].c_str();
}

bool ExportSpec::equals(const ExportSpec& other) const noexcept {
    const Impl& a = *impl_;
    const Impl& b = *other.impl_;
    return a.kind == b.kind && a.file == b.file && a.hasLayer == b.hasLayer && a.layer == b.layer &&
           a.hasRaster == b.hasRaster && a.raster == b.raster && a.measurements == b.measurements;
}

} // namespace slideio::processor
