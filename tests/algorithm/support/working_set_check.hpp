// Requirements: UR-EXE-2, UR-ALG-8
#pragma once

// Checks an operation's working-set declaration against what one apply() or
// accumulate() call allocates through operator new, as the allocation meter
// measures it. The input is built as the framework builds it: the tile grown by
// the operation's halo, one call per plane for a PerChannel operation.

#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/pipeline/operation.hpp>
#include <slideio/processor/pipeline/operation_parameters.hpp>

#include <cstdint>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

namespace slideio::processor::testing_support {

/// The tile sides every check runs at.
extern const int64_t kWorkingSetTileSizes[2];

/// Every sample type.
extern const DataType kAllDataTypes[11];

/// The call measured.
enum class WorkingSetCall { Apply, Accumulate };

/// Calls @p op once per plane (PerChannel) or once on every plane (Joint) on a
/// width x height tile grown by its halo, filled with a pattern that crosses
/// every type's range, and returns the largest peak any one call allocates
/// through operator new. Everything the call is given is built before the
/// meter starts.
/// An operation producing objects emits each object through the run's own tile
/// filter and finishing sink, as within apply() in a run, into a sink that drops
/// it, and each seam fragment to a callback that drops it.
int64_t measureWorkingSetPeak(Operation& op, const std::vector<DataType>& types, int64_t width, int64_t height,
                              WorkingSetCall call);

/// A raster bound to the run under a name, as OperationContext::auxiliary()
/// gives it: one plane of tightly packed samples over the tile grown by the
/// operation's halo.
struct WorkingSetAuxiliary {
    std::string name;
    DataType type = DataType::Unknown;
    std::vector<uint8_t> bytes;
};

/// As measureWorkingSetPeak, on the given input: one plane per type, tightly
/// packed samples of the tile grown by the operation's halo; with
/// @p auxiliary, that raster is bound too.
int64_t measureWorkingSetPeakOn(Operation& op, const std::vector<DataType>& types,
                                const std::vector<std::vector<uint8_t>>& planes, int64_t width, int64_t height,
                                WorkingSetCall call, const WorkingSetAuxiliary* auxiliary = nullptr);

/// What @p op declares for one call on a width x height tile: the halo'd area
/// times its per-pixel figure, plus its per-call figure.
int64_t declaredWorkingSet(const Operation& op, int64_t width, int64_t height);

/// The bytes one call on a plane of @p type, on a width x height tile, takes
/// through an allocator the meter does not see (OpenCV's cv::fastMalloc).
using UnmeteredShare = std::function<int64_t(const Operation& op, DataType type, int64_t width, int64_t height)>;

/// Creates a built-in operation through the catalog.
OperationPtr createOperation(const char* name, const OperationParameters& parameters);

/// Checks that every apply() call's peak is within the declaration less the
/// unmetered share (none when @p share is empty), at every tile size in
/// @p tiles; a fresh operation per tile size, made by @p make.
void expectApplyBounded(const std::string& label, const std::function<OperationPtr()>& make,
                        const std::vector<DataType>& types, const UnmeteredShare& share = UnmeteredShare(),
                        const std::vector<int64_t>& tiles = std::vector<int64_t>(std::begin(kWorkingSetTileSizes),
                                                                                 std::end(kWorkingSetTileSizes)));

/// As expectApplyBounded, for accumulate() on a fresh operation (whose first
/// call creates the channel's histogram), and for apply() once finalised.
void expectGlobalBounded(const std::string& label, const std::function<OperationPtr()>& make,
                         const std::vector<DataType>& types);

} // namespace slideio::processor::testing_support
