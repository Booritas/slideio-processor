// Requirements: UR-IMG-11, UR-ALG-3, UR-ALG-4, UR-EXE-1, UR-IMG-5, UR-STG-3
#include <pipeline/detail/run_plan.hpp>

#include <pipeline/detail/type_contract.hpp>

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace slideio::processor::detail {

namespace {

constexpr int64_t kInt64Max = std::numeric_limits<int64_t>::max();
constexpr const char* kWhere = "Pipeline::validate: ";

[[noreturn]] void invalid(const std::string& message) {
    throw InvalidArgumentError((kWhere + message).c_str());
}

// Non-negative arithmetic that saturates at int64's maximum instead of
// overflowing: a saturated need is still above any ceiling.
int64_t addSat(int64_t a, int64_t b) noexcept { return a > kInt64Max - b ? kInt64Max : a + b; }
int64_t mulSat(int64_t a, int64_t b) noexcept {
    if (a == 0 || b == 0) {
        return 0;
    }
    return a > kInt64Max / b ? kInt64Max : a * b;
}

int64_t planeBytes(const std::vector<DataType>& types) {
    int64_t total = 0;
    for (DataType type : types) {
        total = addSat(total, dataTypeSizeBytes(type));
    }
    return total;
}

std::string bytesText(int64_t bytes) {
    return bytes == kInt64Max ? "more than " + std::to_string(kInt64Max) : std::to_string(bytes);
}

// (d) The selected scene channels, in plane order.
std::vector<int32_t> selectedChannels(const ChannelSelection& selection, int32_t channelCount) {
    std::vector<int32_t> channels;
    if (selection.isAll()) {
        for (int32_t c = 0; c < channelCount; ++c) {
            channels.push_back(c);
        }
        return channels;
    }
    if (selection.count() == 0) {
        invalid("the channel selection is empty");
    }
    for (int32_t i = 0; i < selection.count(); ++i) {
        const int32_t c = selection.at(i);
        if (c < 0 || c >= channelCount) {
            invalid("channel " + std::to_string(c) + " is out of range [0, " + std::to_string(channelCount) + ")");
        }
        channels.push_back(c);
    }
    return channels;
}

// (f) Scene rect, ROI and polygon bounding box, intersected in level-0 pixels.
Rect64 regionAtLevel0(const PipelineSpec& spec, const LevelDims& level0) {
    Rect64 region{0, 0, level0.width, level0.height};
    if (spec.hasRoi()) {
        region = region.intersected(spec.roi());
    }
    if (spec.hasPolygon() && !region.isEmpty()) {
        region = region.intersected(spec.polygon().boundingBox());
    }
    return region;
}

// The region at a level: the level-0 region mapped outward, clipped to the level.
// May be empty.
Rect64 regionAtLevel(const Rect64& region0, int32_t level, const LevelSelector& selector,
                     const std::vector<LevelDims>& levels) {
    const LevelDims& dims = levels[static_cast<size_t>(level)];
    return selector.mapToLevel(region0, level).intersected(Rect64{0, 0, dims.width, dims.height});
}

Rect64 requireRegion(const Rect64& region) {
    if (region.isEmpty()) {
        invalid("the restriction does not intersect the scene");
    }
    return region;
}

std::string planeLabel(size_t step, size_t plane, const std::vector<int32_t>& channels) {
    return step == 0 ? "channel " + std::to_string(channels[plane]) : "plane " + std::to_string(plane);
}

// (g) Walks the chain's plane types; returns them per step.
std::vector<std::vector<DataType>> walkTypes(const std::vector<OperationPtr>& operations,
                                             const std::vector<DataType>& sourceTypes,
                                             const std::vector<int32_t>& channels) {
    for (size_t plane = 0; plane < sourceTypes.size(); ++plane) {
        if (sourceTypes[plane] == DataType::Unknown) {
            const std::string message = std::string(kWhere) + "channel " + std::to_string(channels[plane]) +
                                        " has a type the pipeline cannot read";
            throw UnsupportedDataTypeError(message.c_str(), DataType::Unknown, DataTypeSet());
        }
    }
    std::vector<std::vector<DataType>> steps{sourceTypes};
    for (size_t k = 0; k < operations.size(); ++k) {
        const Operation& op = *operations[k];
        const std::vector<DataType>& in = steps.back();
        const DataTypeSet supported = op.supportedTypes();
        for (DataType type : in) {
            if (!operationAccepts(supported, type)) {
                const std::string message =
                    std::string(kWhere) + op.name() + " does not accept " + dataTypeName(type) + " input";
                throw UnsupportedDataTypeError(message.c_str(), type, supported);
            }
        }
        std::vector<DataType> out;
        if (op.arity() == ChannelArity::Joint) {
            if (op.requiresUniformTypes() &&
                std::any_of(in.begin(), in.end(), [&in](DataType t) { return t != in.front(); })) {
                std::string message =
                    std::string(kWhere) + op.name() + " requires one type across its planes but receives";
                for (size_t plane = 0; plane < in.size(); ++plane) {
                    message += (plane == 0 ? " " : ", ") + planeLabel(k, plane, channels) + " (" +
                               dataTypeName(in[plane]) + ")";
                }
                throw ChannelArityError(message.c_str(), static_cast<int32_t>(in.size()));
            }
            const int32_t count = op.outputPlaneCount(static_cast<int32_t>(in.size()));
            if (count < 1) {
                invalid(std::string(op.name()) + " declares " + std::to_string(count) + " output planes");
            }
            out.assign(static_cast<size_t>(count), op.outputType(in.front()));
        } else {
            for (DataType type : in) {
                out.push_back(op.outputType(type));
            }
        }
        for (DataType type : out) {
            if (dataTypeSizeBytes(type) <= 0) {
                invalid(std::string(op.name()) + " declares no output type for its input");
            }
        }
        steps.push_back(std::move(out));
    }
    return steps;
}

// (h) The layer each operation writes.
std::vector<LayerId> resolveTargetLayers(const std::vector<OperationPtr>& operations, const PipelineSpec& spec,
                                         const std::vector<std::pair<std::string, LayerId>>& layers) {
    std::vector<LayerId> targets;
    for (size_t k = 0; k < operations.size(); ++k) {
        const Operation& op = *operations[k];
        if (!op.producesObjects()) {
            targets.emplace_back();
            continue;
        }
        const OperationParameters& parameters = spec.operationParameters(static_cast<int32_t>(k));
        bool present = false;
        for (int32_t i = 0; i < parameters.count(); ++i) {
            present = present || std::strcmp(parameters.keyAt(i), "targetLayer") == 0;
        }
        if (present) {
            const char* name = parameters.getString("targetLayer", nullptr);
            if (name == nullptr) {
                invalid(std::string("the \"targetLayer\" parameter of ") + op.name() + " is not a string");
            }
            const auto bound = std::find_if(layers.begin(), layers.end(),
                                            [name](const std::pair<std::string, LayerId>& l) { return l.first == name; });
            if (bound == layers.end()) {
                invalid(std::string(op.name()) + " targets layer \"" + name + "\", which is not bound");
            }
            targets.push_back(bound->second);
        } else if (layers.size() == 1) {
            targets.push_back(layers.front().second);
        } else if (layers.empty()) {
            invalid(std::string(op.name()) + " has no target layer: no layer is bound");
        } else {
            invalid(std::string(op.name()) + " has no target layer and " + std::to_string(layers.size()) +
                    " layers are bound; name one with its \"targetLayer\" parameter");
        }
    }
    return targets;
}

Halo sumHalos(const std::vector<OperationPtr>& operations) {
    int64_t left = 0, top = 0, right = 0, bottom = 0;
    for (const OperationPtr& op : operations) {
        const Halo h = op->halo();
        if (h.left < 0 || h.top < 0 || h.right < 0 || h.bottom < 0) {
            invalid(std::string(op->name()) + " declares a negative halo");
        }
        left += h.left;
        top += h.top;
        right += h.right;
        bottom += h.bottom;
    }
    constexpr int64_t kMax = std::numeric_limits<int32_t>::max();
    if (left > kMax || top > kMax || right > kMax || bottom > kMax) {
        invalid("the chain's halo does not fit int32");
    }
    return Halo{static_cast<int32_t>(left), static_cast<int32_t>(top), static_cast<int32_t>(right),
                static_cast<int32_t>(bottom)};
}

} // namespace

RunPlan buildRunPlan(const RunPlanInputs& inputs) {
    const PipelineSpec& spec = *inputs.spec;
    const std::vector<LevelDims>& levels = *inputs.levels;
    const LevelSelector& selector = *inputs.selector;
    const OperationCatalog& catalog = OperationCatalog::instance();

    // (b) At least one operation; (c) every name registered.
    if (spec.operationCount() == 0) {
        invalid("the spec has no operation");
    }
    for (int32_t k = 0; k < spec.operationCount(); ++k) {
        if (!catalog.isRegistered(spec.operationName(k))) {
            invalid(std::string("operation \"") + spec.operationName(k) + "\" is not registered");
        }
    }

    // (d) Channels; (e) an explicit level.
    const std::vector<DataType>& channelTypes = *inputs.channelTypes;
    const std::vector<int32_t> channels =
        selectedChannels(spec.channels(), static_cast<int32_t>(channelTypes.size()));
    const int32_t levelCount = static_cast<int32_t>(levels.size());
    if (spec.analysisLevel() >= levelCount) {
        invalid("analysis level " + std::to_string(spec.analysisLevel()) + " is out of range [0, " +
                std::to_string(levelCount) + ")");
    }

    // (f) The region.
    const Rect64 region0 = regionAtLevel0(spec, levels.front());
    if (region0.isEmpty()) {
        invalid("the restriction does not intersect the scene");
    }

    // (g) Types, on operations created from the spec.
    std::vector<OperationPtr> operations;
    for (int32_t k = 0; k < spec.operationCount(); ++k) {
        operations.push_back(catalog.create(spec.operationName(k), spec.operationParameters(k)));
    }
    std::vector<DataType> sourceTypes;
    for (int32_t c : channels) {
        sourceTypes.push_back(channelTypes[static_cast<size_t>(c)]);
    }
    std::vector<std::vector<DataType>> planeTypes = walkTypes(operations, sourceTypes, channels);

    // (h) Target layers.
    std::vector<LayerId> targetLayers = resolveTargetLayers(operations, spec, *inputs.layers);

    // Bound auxiliary rasters: each declares at least one plane of a type with a size.
    for (const auto& auxiliary : *inputs.auxiliaries) {
        if (auxiliary.second->planeCount() <= 0) {
            invalid("auxiliary raster \"" + auxiliary.first + "\" declares no plane");
        }
        if (dataTypeSizeBytes(auxiliary.second->planeType()) <= 0) {
            invalid("auxiliary raster \"" + auxiliary.first + "\" declares a plane type without a size");
        }
    }

    // (i) Whole-level layout, budget and level.
    const auto whole = std::find_if(operations.begin(), operations.end(), [](const OperationPtr& op) {
        return op->tileRequirement() == TileRequirement::WholeLevel;
    });
    const bool wholeLevel = whole != operations.end();
    int64_t bytesPerPixel = 0;
    for (const std::vector<DataType>& step : planeTypes) {
        bytesPerPixel = addSat(bytesPerPixel, planeBytes(step));
    }
    int64_t wholeBytesPerPixel = bytesPerPixel;
    for (const OperationPtr& op : operations) {
        wholeBytesPerPixel = addSat(wholeBytesPerPixel, std::max<int64_t>(0, op->workingSetBytesPerLevelPixel()));
    }
    auto wholeNeed = [&](const Rect64& region) {
        return mulSat(mulSat(region.width, region.height), wholeBytesPerPixel);
    };
    auto tooBig = [&](int32_t level, int64_t needed, const char* prefix) {
        invalid(std::string(prefix) + (*whole)->name() + " needs level " + std::to_string(level) +
                " whole in memory: " + bytesText(needed) + " bytes, above the memory ceiling of " +
                std::to_string(inputs.ceilingBytes) + " bytes");
    };

    // What one tile's buffers need: every step's planes, the reader's interleave
    // buffer, every auxiliary's planes and the restriction masks, over the tile
    // grown by the chain's halo.
    const Halo chainHalo = sumHalos(operations);
    int64_t tilePixelBytes = bytesPerPixel;
    // The reader reads every channel in one call when their types agree, into a
    // buffer as large as all the source planes; otherwise one channel per call,
    // into a buffer as large as the widest source plane.
    const bool uniformSource = std::all_of(sourceTypes.begin(), sourceTypes.end(),
                                           [&sourceTypes](DataType t) { return t == sourceTypes.front(); });
    int64_t widestSource = 0;
    for (DataType type : sourceTypes) {
        widestSource = std::max<int64_t>(widestSource, dataTypeSizeBytes(type));
    }
    tilePixelBytes = addSat(tilePixelBytes, uniformSource ? planeBytes(sourceTypes) : widestSource);
    for (const auto& auxiliary : *inputs.auxiliaries) {
        const int64_t count = auxiliary.second->planeCount();
        tilePixelBytes = addSat(tilePixelBytes, mulSat(count, dataTypeSizeBytes(auxiliary.second->planeType())));
    }
    const bool restricted = spec.hasRoi() || spec.hasPolygon() || inputs.hasMaskSource;
    // intersect() holds the tile mask plus one temporary mask of the same size while it
    // applies a polygon or a mask source (one at a time): 2 bytes; the ROI alone needs 1.
    tilePixelBytes = addSat(tilePixelBytes, restricted ? (spec.hasPolygon() || inputs.hasMaskSource ? 2 : 1) : 0);
    auto haloSide = [](int64_t side, int32_t a, int32_t b) { return addSat(side, int64_t(a) + b); };
    auto tileNeed = [&](int64_t tileW, int64_t tileH) {
        return mulSat(mulSat(haloSide(tileW, chainHalo.left, chainHalo.right),
                             haloSide(tileH, chainHalo.top, chainHalo.bottom)),
                      tilePixelBytes);
    };

    int32_t level = spec.analysisLevel();
    Rect64 region{};
    if (level >= 0) {
        region = requireRegion(regionAtLevel(region0, level, selector, levels));
        if (wholeLevel && wholeNeed(region) > inputs.ceilingBytes) {
            tooBig(level, wholeNeed(region), "");
        }
    } else if (!wholeLevel) {
        level = 0;
        region = requireRegion(regionAtLevel(region0, level, selector, levels));
    } else {
        // The finest level whose single tile passes both the whole-level budget and the per-tile bytes. Levels whose
        // mapped region is empty are skipped.
        int32_t coarsest = -1;
        Rect64 coarsestRegion{};
        for (int32_t candidate = 0; candidate < levelCount && level < 0; ++candidate) {
            const Rect64 candidateRegion = regionAtLevel(region0, candidate, selector, levels);
            if (candidateRegion.isEmpty()) {
                continue;
            }
            coarsest = candidate;
            coarsestRegion = candidateRegion;
            if (wholeNeed(candidateRegion) <= inputs.ceilingBytes &&
                tileNeed(candidateRegion.width, candidateRegion.height) <= inputs.ceilingBytes) {
                level = candidate;
                region = candidateRegion;
            }
        }
        if (level < 0) {
            if (coarsest < 0) {
                invalid("the restriction does not intersect the scene");
            }
            if (wholeNeed(coarsestRegion) > inputs.ceilingBytes) {
                tooBig(coarsest, wholeNeed(coarsestRegion), "no level fits the memory ceiling: ");
            }
            // The whole-level budget fits there but the tile bytes do not: the tile check below reports it.
            level = coarsest;
            region = coarsestRegion;
        }
    }

    // The grid, one tile for a whole-level chain.
    int32_t tileWidth = 0;
    int32_t tileHeight = 0;
    if (wholeLevel) {
        constexpr int64_t kMax = std::numeric_limits<int32_t>::max();
        if (region.width > kMax || region.height > kMax) {
            invalid(std::string((*whole)->name()) + " needs level " + std::to_string(level) +
                    " as one tile, whose side does not fit int32");
        }
        tileWidth = static_cast<int32_t>(region.width);
        tileHeight = static_cast<int32_t>(region.height);
    } else {
        tileSizeOfLevel(levels[static_cast<size_t>(level)], *inputs.config, tileWidth, tileHeight);
    }
    TileGrid grid(region, level, tileWidth, tileHeight, selector);

    // One tile's buffers against the ceiling.
    const int64_t tileW = std::min<int64_t>(tileWidth, region.width);
    const int64_t tileH = std::min<int64_t>(tileHeight, region.height);
    const int64_t tileBytes = tileNeed(tileW, tileH);
    if (tileBytes > inputs.ceilingBytes) {
        invalid("a tile of " + std::to_string(tileW) + " x " + std::to_string(tileH) + " level pixels (" +
                std::to_string(haloSide(tileW, chainHalo.left, chainHalo.right)) + " x " +
                std::to_string(haloSide(tileH, chainHalo.top, chainHalo.bottom)) + " with the chain's halo) needs " +
                bytesText(tileBytes) + " bytes, above the memory ceiling of " + std::to_string(inputs.ceilingBytes) +
                " bytes");
    }

    std::vector<size_t> globalContextPositions;
    for (size_t k = 0; k < operations.size(); ++k) {
        if (operations[k]->needsGlobalContext()) {
            globalContextPositions.push_back(k);
        }
    }

    return RunPlan{level,
                   selector.scaleOfLevel(level),
                   std::move(grid),
                   std::move(operations),
                   std::move(planeTypes),
                   channels,
                   std::move(targetLayers),
                   chainHalo,
                   std::move(globalContextPositions),
                   restricted,
                   tileBytes,
                   Rect64{0, 0, levels[static_cast<size_t>(level)].width, levels[static_cast<size_t>(level)].height}};
}

} // namespace slideio::processor::detail
