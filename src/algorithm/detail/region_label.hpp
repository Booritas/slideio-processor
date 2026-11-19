// Requirements: UR-SEG-2, UR-SEG-3
#pragma once

#include <slideio/processor/core/error.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace slideio::processor::detail {

/// Union-find over provisional labels 1..: the root of a set is its smallest
/// label, so it is the label its first pixel received in row-major order.
class LabelParents {
public:
    explicit LabelParents(size_t capacity) : parent_(capacity + 1, 0) {}

    int32_t make() {
        const int32_t label = ++count_;
        parent_[static_cast<size_t>(label)] = label;
        return label;
    }

    int32_t find(int32_t label) {
        while (parent_[static_cast<size_t>(label)] != label) {
            const int32_t up = parent_[static_cast<size_t>(label)];
            parent_[static_cast<size_t>(label)] = parent_[static_cast<size_t>(up)];
            label = up;
        }
        return label;
    }

    /// Joins the sets of a and b; returns the root of the joined set.
    int32_t unite(int32_t a, int32_t b) {
        const int32_t ra = find(a);
        const int32_t rb = find(b);
        if (ra == rb) {
            return ra;
        }
        if (ra < rb) {
            parent_[static_cast<size_t>(rb)] = ra;
            return ra;
        }
        parent_[static_cast<size_t>(ra)] = rb;
        return rb;
    }

    /// Replaces every entry by its set's final label, numbered 1.. in the order
    /// of the roots. Every non-root entry points to a smaller label, so
    /// ascending order resolves a label after the one it points to; the final
    /// labels are held negated until then.
    void resolve() {
        int32_t next = 0;
        for (int32_t label = 1; label <= count_; ++label) {
            const int32_t up = parent_[static_cast<size_t>(label)];
            parent_[static_cast<size_t>(label)] = up == label ? -(++next) : parent_[static_cast<size_t>(up)];
        }
        for (int32_t label = 1; label <= count_; ++label) {
            parent_[static_cast<size_t>(label)] = -parent_[static_cast<size_t>(label)];
        }
    }

    int32_t finalLabel(int32_t label) const { return parent_[static_cast<size_t>(label)]; }

private:
    std::vector<int32_t> parent_;
    int32_t count_ = 0;
};

/// Labels the regions of a width x height tile, two-pass union-find: writes
/// each pixel's final label into @p labels, row y at labels + y * stride: 1..K
/// in the order regions are first met row by row, 0 for background. A pixel
/// belongs to a region when foreground(x, y); two foreground neighbours (side
/// by side, or also diagonal with @p eightConnected) are in one region when
/// joins(x, y, nx, ny) holds for the pixel and its earlier neighbour. Each
/// region is connected, as traceRegions() requires.
///
/// @p maximumLabels bounds how many provisional labels the tile can start;
/// the union-find takes 4 bytes per label, freed on return. Throws
/// InvalidArgumentError "<operation>: the tile has too many pixels to label"
/// when the bound does not fit an int32.
template <typename Foreground, typename Joins>
void labelRegions(int32_t* labels, int64_t stride, int64_t width, int64_t height, bool eightConnected,
                  int64_t maximumLabels, const char* operation, Foreground foreground, Joins joins) {
    if (maximumLabels >= std::numeric_limits<int32_t>::max()) {
        throw InvalidArgumentError((std::string(operation) + ": the tile has too many pixels to label").c_str());
    }
    LabelParents parents(static_cast<size_t>(maximumLabels));
    for (int64_t y = 0; y < height; ++y) {
        int32_t* row = labels + y * stride;
        const int32_t* above = y > 0 ? labels + (y - 1) * stride : nullptr;
        for (int64_t x = 0; x < width; ++x) {
            if (!foreground(x, y)) {
                row[x] = 0;
                continue;
            }
            int32_t label = 0;
            const auto join = [&](int32_t neighbour, int64_t nx, int64_t ny) {
                if (neighbour > 0 && joins(x, y, nx, ny)) {
                    label = label == 0 ? parents.find(neighbour) : parents.unite(label, neighbour);
                }
            };
            if (x > 0) {
                join(row[x - 1], x - 1, y);
            }
            if (above != nullptr) {
                join(above[x], x, y - 1);
                if (eightConnected) {
                    if (x > 0) {
                        join(above[x - 1], x - 1, y - 1);
                    }
                    if (x + 1 < width) {
                        join(above[x + 1], x + 1, y - 1);
                    }
                }
            }
            row[x] = label == 0 ? parents.make() : label;
        }
    }
    parents.resolve();
    for (int64_t y = 0; y < height; ++y) {
        int32_t* row = labels + y * stride;
        for (int64_t x = 0; x < width; ++x) {
            row[x] = row[x] == 0 ? 0 : parents.finalLabel(row[x]);
        }
    }
}

} // namespace slideio::processor::detail
