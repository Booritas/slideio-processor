// Requirements: UR-EXE-2, UR-PIX-6
#pragma once

namespace slideio::processor::detail {

/// Marks the calling thread as holding a tile's memory reservation while the tile
/// is evaluated, so memory reserved further down the same call (a lazy scene read
/// from inside the tile) never waits for bytes only tiles can free. Nests: the
/// previous mark is restored on destruction, on every exit path. Only the engine's
/// own sources use it, so the thread-local state lives in this one inline function.
class TileReservationMark {
public:
    TileReservationMark() noexcept : previous_(flag()) { flag() = true; }
    ~TileReservationMark() { flag() = previous_; }
    TileReservationMark(const TileReservationMark&) = delete;
    TileReservationMark& operator=(const TileReservationMark&) = delete;

    /// True while the calling thread holds a tile reservation.
    static bool held() noexcept { return flag(); }

private:
    static bool& flag() noexcept {
        thread_local bool marked = false;
        return marked;
    }

    bool previous_;
};

} // namespace slideio::processor::detail
