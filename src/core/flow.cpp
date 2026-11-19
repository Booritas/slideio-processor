// Requirements: UR-RES-2, UR-RES-5, UR-RES-6, UR-EXT-4
#include <slideio/processor/core/flow.hpp>

namespace slideio::processor {

ObjectSink::~ObjectSink() = default;

bool ObjectSink::emitKept(const ObjectRecord& record) {
    emit(record);
    return true;
}

bool ObjectSink::emitOwned(ObjectRecord&& record) { return emitKept(record); }

EdgeSink::~EdgeSink() = default;
RasterSink::~RasterSink() = default;
AnnotationSink::~AnnotationSink() = default;
Cursor::~Cursor() = default;
ObjectSource::~ObjectSource() = default;
TileMaskSource::~TileMaskSource() = default;
TileRasterSource::~TileRasterSource() = default;

} // namespace slideio::processor
