// Requirements: UR-RES-9, UR-RES-15, UR-SEG-6
#include <slideio/processor/reporting/column_schema.hpp>

#include <slideio/processor/core/error.hpp>

#include "reporting/detail/columns.hpp"

#include <algorithm>
#include <cstring>

namespace slideio::processor {

namespace {

void rebuild(ColumnSchema::Impl& s) {
    std::vector<std::string> names;
    for (const detail::ResolvedColumn& c : detail::fixedColumns(s)) names.push_back(c.name);
    for (const std::string& m : s.measurements) names.push_back(m);
    s.names = std::move(names);
}

// Applies a change to a copy and commits it only when everything succeeded, so a
// std::bad_alloc leaves the schema unchanged.
template <class F> void modify(std::unique_ptr<ColumnSchema::Impl>& impl, F change) {
    auto copy = std::make_unique<ColumnSchema::Impl>(*impl);
    change(*copy);
    rebuild(*copy);
    impl = std::move(copy);
}

std::unique_ptr<ColumnSchema::Impl> makeDefault() {
    auto impl = std::make_unique<ColumnSchema::Impl>();
    rebuild(*impl);
    return impl;
}

bool needsQuoting(const std::string& s) {
    // '#' too: pandas.read_csv(comment='#') cuts a line at an unquoted '#'.
    return s.find_first_of(",\"\r\n#") != std::string::npos;
}

} // namespace

ColumnSchema::ColumnSchema() : impl_(makeDefault()) {}
ColumnSchema::ColumnSchema(const ColumnSchema& other)
    : impl_(std::make_unique<Impl>(*other.impl_)) {}
ColumnSchema::ColumnSchema(ColumnSchema&& other) {
    // The default is built first, so a throw leaves the source untouched and valid.
    auto fresh = makeDefault();
    impl_ = std::move(other.impl_);
    other.impl_ = std::move(fresh);
}
ColumnSchema& ColumnSchema::operator=(const ColumnSchema& other) {
    if (this != &other) impl_ = std::make_unique<Impl>(*other.impl_);
    return *this;
}
ColumnSchema& ColumnSchema::operator=(ColumnSchema&& other) {
    if (this != &other) {
        auto fresh = makeDefault();
        impl_ = std::move(other.impl_);
        other.impl_ = std::move(fresh);
    }
    return *this;
}
ColumnSchema::~ColumnSchema() = default;

ColumnSchema ColumnSchema::standard() { return ColumnSchema(); }

ColumnSchema& ColumnSchema::idFormat(IdFormat format) {
    modify(impl_, [&](Impl& s) { s.idFormat = format; });
    return *this;
}
ColumnSchema& ColumnSchema::units(UnitPolicy policy) {
    modify(impl_, [&](Impl& s) { s.units = policy; });
    return *this;
}
ColumnSchema& ColumnSchema::includeLayer(bool include) {
    modify(impl_, [&](Impl& s) { s.layer = include; });
    return *this;
}
ColumnSchema& ColumnSchema::includeStage(bool include) {
    modify(impl_, [&](Impl& s) { s.stage = include; });
    return *this;
}
ColumnSchema& ColumnSchema::includeLabel(bool include) {
    modify(impl_, [&](Impl& s) { s.label = include; });
    return *this;
}
ColumnSchema& ColumnSchema::labelPolicy(LabelPolicy policy) {
    modify(impl_, [&](Impl& s) { s.labelPolicy = policy; });
    return *this;
}
ColumnSchema& ColumnSchema::includeBoundingBox(bool include) {
    modify(impl_, [&](Impl& s) { s.boundingBox = include; });
    return *this;
}
ColumnSchema& ColumnSchema::includeGeometryWkt(bool include) {
    modify(impl_, [&](Impl& s) { s.geometryWkt = include; });
    return *this;
}
ColumnSchema& ColumnSchema::includeParent(bool include) {
    modify(impl_, [&](Impl& s) { s.parent = include; });
    return *this;
}

ColumnSchema& ColumnSchema::measurement(const char* name) {
    if (name == nullptr || name[0] == '\0')
        throw InvalidArgumentError("ColumnSchema::measurement: the name is null or empty");
    const std::string base(name);
    if (detail::isTwinName(base))
        throw InvalidArgumentError(
            "ColumnSchema::measurement: give the base name; units are chosen by the unit policy");
    if (detail::isFixedColumnName(base))
        throw InvalidArgumentError(
            "ColumnSchema::measurement: the name is that of a fixed column");
    const auto& held = impl_->measurements;
    if (std::find(held.begin(), held.end(), base) != held.end())
        throw InvalidArgumentError("ColumnSchema::measurement: the measurement was already added");
    modify(impl_, [&](Impl& s) { s.measurements.push_back(base); });
    return *this;
}

ColumnSchema& ColumnSchema::allMeasurements() {
    modify(impl_, [&](Impl& s) { s.allMeasurements = true; });
    return *this;
}

int32_t ColumnSchema::columnCount() const noexcept {
    return static_cast<int32_t>(impl_->names.size());
}

const char* ColumnSchema::columnNameAt(int32_t index) const {
    if (index < 0 || index >= columnCount())
        throw InvalidArgumentError("ColumnSchema::columnNameAt: index out of range");
    return impl_->names[static_cast<size_t>(index)].c_str();
}

int32_t ColumnSchema::toHeaderLine(char* buffer, int32_t capacity) const {
    std::string line;
    for (size_t i = 0; i < impl_->names.size(); ++i) {
        if (i > 0) line += ',';
        const std::string& name = impl_->names[i];
        if (needsQuoting(name)) {
            line += '"';
            for (char c : name) {
                if (c == '"') line += '"';
                line += c;
            }
            line += '"';
        } else {
            line += name;
        }
    }
    if (buffer != nullptr && capacity > 0) {
        const size_t room = static_cast<size_t>(capacity) - 1;
        const size_t n = line.size() < room ? line.size() : room;
        std::memcpy(buffer, line.data(), n);
        buffer[n] = '\0';
    }
    return static_cast<int32_t>(line.size());
}

} // namespace slideio::processor
