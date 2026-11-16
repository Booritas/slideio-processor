// Requirements: UR-SEG-3, UR-SEG-5
#include <pipeline/detail/combine_partials.hpp>

#include <slideio/processor/core/error.hpp>

#include <algorithm>

namespace slideio::processor::detail {

IntensityPartial combinePartials(const IntensityPartial& a, const IntensityPartial& b) {
    if (a.channel != b.channel) {
        throw InvalidArgumentError("combinePartials: the partials are of different channels");
    }
    if (a.count == 0) {
        return b;
    }
    if (b.count == 0) {
        return a;
    }
    IntensityPartial c = a;
    c.count = a.count + b.count;
    c.sum = a.sum + b.sum;
    c.sumOfSquares = a.sumOfSquares + b.sumOfSquares;
    c.minimum = std::min(a.minimum, b.minimum);
    c.maximum = std::max(a.maximum, b.maximum);
    return c;
}

} // namespace slideio::processor::detail
