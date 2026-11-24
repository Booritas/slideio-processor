// Requirements: UR-API-2, UR-RES-7, UR-RES-8, UR-RES-9, UR-RES-11
#include <slideio/processor/run/workspace_export.hpp>
#include "core/detail/diagnostics.hpp"

#include <pipeline/detail/level_table.hpp>

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/core/object_query.hpp>
#include <slideio/processor/masks/label_scene.hpp>
#include <slideio/processor/masks/mask_writer.hpp>
#include <slideio/processor/reporting/column_schema.hpp>
#include <slideio/processor/reporting/csv_writer.hpp>
#include <slideio/processor/reporting/geojson_exporter.hpp>
#include <slideio/processor/reporting/raster_tiff_writer.hpp>
#include <slideio/processor/storage/layer_registry.hpp>
#include <slideio/processor/storage/raster_store.hpp>
#include <slideio/processor/storage/store.hpp>

#include <slideio/slideio/scene.hpp>

#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <string>
#include <tuple>
#include <vector>

namespace slideio::processor {

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

[[noreturn]] void refuse(const std::string& message) {
    throw InvalidArgumentError(("writeExport: " + message).c_str());
}

// Translates what SlideIO throws while describing the scene of `slide`, which the message names;
// SlideIO's text, unaltered (it may name the slide's path too), is the cause.
template <typename F>
auto fromScene(const std::string& slide, F&& call) -> decltype(call()) {
    try {
        return call();
    } catch (const ProcessorError&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        const std::string cause = detail::diagnosticText(e);
        ErrorContext context;
        context.cause = cause.c_str();
        throw InternalError(detail::withPath("writeExport: SlideIO failed to describe the scene", slide).c_str(),
                            context);
    }
}

LayerId layerOf(Workspace& workspace, const ExportSpec& spec) {
    LayerId layer;
    if (!workspace.layers().find(spec.layer(), layer)) {
        refuse(std::string("the ") + kindName(spec.kind()) + " export names the layer '" + spec.layer() +
               "', which the workspace does not hold");
    }
    return layer;
}

} // namespace

ExportResult writeExport(Workspace& workspace, const ExportSpec& spec, const char* path, const char* slideId,
                         ProgressObserver* observer, const CancellationToken& token) {
    if (path == nullptr || path[0] == '\0') refuse("the path is null or empty");
    const std::string id = slideId != nullptr ? slideId : "";
    const std::string slidePath = workspace.slidePath();
    Store& store = *workspace.store();
    switch (spec.kind()) {
        case ExportKind::Csv: {
            const LayerId layer = layerOf(workspace, spec);
            ColumnSchema schema = ColumnSchema::standard();
            if (spec.measurementCount() == 0) {
                schema.allMeasurements();
            } else {
                for (int32_t m = 0; m < spec.measurementCount(); ++m) schema.measurement(spec.measurementAt(m));
            }
            CsvWriter writer(path);
            writer.schema(schema).slideId(id.c_str());
            return writer.write(store, ObjectQuery().layer(layer), observer, token);
        }
        case ExportKind::GeoJson: {
            const LayerId layer = layerOf(workspace, spec);
            GeoJsonExporter writer(path);
            writer.slideId(id.c_str());
            return writer.write(store, ObjectQuery().layer(layer), observer, token);
        }
        case ExportKind::LabelMask: {
            const LayerId layer = layerOf(workspace, spec);
            const std::shared_ptr<slideio::Scene> scene = workspace.scene();
            const std::tuple<int, int, int, int> rect = fromScene(slidePath, [&] { return scene->getRect(); });
            const std::tuple<double, double> metres = fromScene(slidePath, [&] { return scene->getResolution(); });
            const Rect64 region{0, 0, std::get<2>(rect), std::get<3>(rect)};
            const std::shared_ptr<slideio::Scene> labels = rasteriseLabels(store, ObjectQuery().layer(layer), region);
            MaskWriter writer(path);
            if (std::get<0>(metres) > 0.0) writer.resolution(std::get<0>(metres) * 1e6);
            return writer.write(labels, observer, token);
        }
        case ExportKind::RasterTiff: {
            RasterRegistry& rasters = store.rasters();
            RasterId raster;
            if (!rasters.find(spec.raster(), raster)) {
                refuse(std::string("the rasterTiff export names the raster '") + spec.raster() +
                       "', which the workspace does not hold");
            }
            if (!rasters.isPopulated(raster)) {
                refuse(std::string("the rasterTiff export names the raster '") + spec.raster() +
                       "', which no stage has written");
            }
            const int32_t level = rasters.writtenAtLevel(raster);
            const std::shared_ptr<slideio::Scene> scene = workspace.scene();
            const std::vector<detail::LevelDims> table =
                fromScene(slidePath, [&] { return detail::levelTable(*scene); });
            if (level < 0 || static_cast<size_t>(level) >= table.size()) {
                throw InternalError(("writeExport: the raster '" + std::string(spec.raster()) +
                                     "' was written at a level the scene does not have")
                                        .c_str());
            }
            const double scale = detail::scaleOfLevel(table, level);
            const std::tuple<double, double> metres = fromScene(slidePath, [&] { return scene->getResolution(); });
            const TileRasterSourcePtr source = rasters.sourceFor(raster);
            RasterTiffWriter writer(path);
            writer.name(spec.raster());
            if (std::get<0>(metres) > 0.0) writer.resolution(std::get<0>(metres) * 1e6 * scale);
            const Rect64 region{0, 0, table.front().width, table.front().height};
            return writer.write(*source, region, scale, observer, token);
        }
    }
    throw InternalError("writeExport: an export has an unknown kind");
}

} // namespace slideio::processor
