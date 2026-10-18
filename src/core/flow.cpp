#include <slideio/processor/core/flow.hpp>

namespace slideio::processor {

ObjectSink::~ObjectSink() = default;
EdgeSink::~EdgeSink() = default;
RasterSink::~RasterSink() = default;
AnnotationSink::~AnnotationSink() = default;
Cursor::~Cursor() = default;
ObjectSource::~ObjectSource() = default;
TileMaskSource::~TileMaskSource() = default;
TileRasterSource::~TileRasterSource() = default;

} // namespace slideio::processor
