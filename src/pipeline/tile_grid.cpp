// Requirements: UR-IMG-4, UR-EXE-6, UR-EXT-3, UR-SEG-3, UR-QA-2, UR-QA-3, UR-IMG-5, UR-IMG-1
#include <slideio/processor/pipeline/tile_grid.hpp>

#include <slideio/processor/core/error.hpp>

#include <core/detail/level_mapping.hpp>

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace slideio::processor {

class TileGrid::Impl {
public:
    Rect64 region{0, 0, 0, 0};
    int32_t level = 0;
    double scale = 1.0;
    int64_t tileWidth = 1;
    int64_t tileHeight = 1;
    int64_t columns = 0;
    int64_t rows = 0;

    void requireIndex(int64_t index, const char* method) const {
        if (index < 0 || index >= columns * rows) {
            throw InvalidArgumentError((std::string("TileGrid::") + method + ": index " + std::to_string(index) +
                                        " is out of range [0, " + std::to_string(columns * rows) + ")")
                                           .c_str());
        }
    }

    Rect64 tileAt(int64_t index, const char* method) const {
        requireIndex(index, method);
        const int64_t x = region.x + (index % columns) * tileWidth;
        const int64_t y = region.y + (index / columns) * tileHeight;
        return Rect64{x, y, std::min(tileWidth, region.right() - x), std::min(tileHeight, region.bottom() - y)};
    }

    Rect64 tileWithHaloAt(int64_t index, const Halo& halo, const char* method) const {
        if (halo.left < 0 || halo.top < 0 || halo.right < 0 || halo.bottom < 0) {
            throw InvalidArgumentError(
                (std::string("TileGrid::") + method + ": every side of the halo must be non-negative").c_str());
        }
        const Rect64 t = tileAt(index, method);
        return Rect64{t.x - halo.left, t.y - halo.top, t.width + halo.left + halo.right,
                      t.height + halo.top + halo.bottom};
    }
};

TileGrid::TileGrid(const Rect64& regionAtLevel, int32_t level, int32_t tileWidth, int32_t tileHeight,
                   const LevelSelector& selector) {
    if (tileWidth < 1 || tileHeight < 1) {
        throw InvalidArgumentError(("TileGrid: the tile size " + std::to_string(tileWidth) + "x" +
                                    std::to_string(tileHeight) + " must be at least 1x1")
                                       .c_str());
    }
    auto impl = std::make_shared<Impl>();
    // Throws InvalidArgumentError for a level out of range.
    impl->scale = selector.scaleOfLevel(level);
    impl->region = regionAtLevel;
    impl->level = level;
    impl->tileWidth = tileWidth;
    impl->tileHeight = tileHeight;
    if (!regionAtLevel.isEmpty()) {
        // Written as a quotient plus a remainder so that a region near INT64_MAX cannot overflow.
        impl->columns = regionAtLevel.width / tileWidth + (regionAtLevel.width % tileWidth != 0 ? 1 : 0);
        impl->rows = regionAtLevel.height / tileHeight + (regionAtLevel.height % tileHeight != 0 ? 1 : 0);
        if (impl->columns > std::numeric_limits<int64_t>::max() / impl->rows) {
            throw InvalidArgumentError("TileGrid: the tile count does not fit int64");
        }
    }
    impl_ = std::move(impl);
}

TileGrid::TileGrid(const TileGrid& other) noexcept = default;
// The state is immutable and shared, so a move copies the pointer and the source keeps its value.
TileGrid::TileGrid(TileGrid&& other) noexcept : impl_(other.impl_) {}
TileGrid& TileGrid::operator=(const TileGrid& other) noexcept = default;
TileGrid& TileGrid::operator=(TileGrid&& other) noexcept {
    impl_ = other.impl_;
    return *this;
}
TileGrid::~TileGrid() = default;

int32_t TileGrid::level() const noexcept {
    return impl_->level;
}

int64_t TileGrid::tileCount() const noexcept {
    return impl_->columns * impl_->rows;
}

int64_t TileGrid::columns() const noexcept {
    return impl_->columns;
}

int64_t TileGrid::rows() const noexcept {
    return impl_->rows;
}

Rect64 TileGrid::tileRectAtLevel(int64_t index) const {
    return impl_->tileAt(index, "tileRectAtLevel");
}

Rect64 TileGrid::tileRectAtLevelWithHalo(int64_t index, const Halo& halo) const {
    return impl_->tileWithHaloAt(index, halo, "tileRectAtLevelWithHalo");
}

Rect64 TileGrid::tileRect(int64_t index) const {
    return detail::rectToLevel0(impl_->tileAt(index, "tileRect"), impl_->scale);
}

Rect64 TileGrid::tileRectWithHalo(int64_t index, const Halo& halo) const {
    return detail::rectToLevel0(impl_->tileWithHaloAt(index, halo, "tileRectWithHalo"), impl_->scale);
}

int64_t TileGrid::tileIndex(int64_t column, int64_t row) const {
    if (column < 0 || column >= impl_->columns || row < 0 || row >= impl_->rows) {
        throw InvalidArgumentError(("TileGrid::tileIndex: column " + std::to_string(column) + ", row " +
                                    std::to_string(row) + " is outside the " + std::to_string(impl_->columns) +
                                    "x" + std::to_string(impl_->rows) + " grid")
                                       .c_str());
    }
    return row * impl_->columns + column;
}

bool TileGrid::isEdgeTile(int64_t index) const {
    impl_->requireIndex(index, "isEdgeTile");
    const int64_t column = index % impl_->columns;
    const int64_t row = index / impl_->columns;
    return column == 0 || row == 0 || column == impl_->columns - 1 || row == impl_->rows - 1;
}

} // namespace slideio::processor
