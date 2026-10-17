// Requirements: UR-IMG-6, UR-IMG-7, UR-ALG-2
#pragma once

#include <slideio/processor/core/export.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

/**
 * @brief An ordered selection of image channels: either an explicit list or
 * "all channels".
 *
 * A default-built selection is an empty explicit list. all() selects every
 * channel, has count() 0 and cannot be indexed or extended. A moved-from
 * selection is valid and empty.
 * @par Thread-safety
 * ThreadCompatible: const members may run concurrently; any mutation needs
 * exclusive access.
 * @par Requirements
 * UR-IMG-6, UR-IMG-7, UR-ALG-2
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS ChannelSelection {
public:
    /**
     * @brief Builds an empty explicit selection.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ChannelSelection();
    /**
     * @brief Copies a selection.
     * @param other The selection to copy.
     * @throws std::bad_alloc When memory is exhausted.
     */
    ChannelSelection(const ChannelSelection& other);
    /**
     * @brief Moves a selection. The source is left empty.
     * @param other The selection to move from.
     * @throws Nothing.
     */
    ChannelSelection(ChannelSelection&& other) noexcept;
    /**
     * @brief Copy-assigns a selection.
     * @param other The selection to copy.
     * @return This object.
     * @throws std::bad_alloc When memory is exhausted; this object is unchanged.
     */
    ChannelSelection& operator=(const ChannelSelection& other);
    /**
     * @brief Move-assigns a selection. The source is left empty.
     * @param other The selection to move from.
     * @return This object.
     * @throws Nothing.
     */
    ChannelSelection& operator=(ChannelSelection&& other) noexcept;
    /** @brief Destroys the selection. */
    ~ChannelSelection();

    /**
     * @brief Builds a selection of one channel.
     * @param channel The channel index, at least 0.
     * @return The selection.
     * @throws InvalidArgumentError When channel is negative.
     */
    static ChannelSelection single(int32_t channel);
    /**
     * @brief Builds a selection of consecutive channels.
     * @param first The first channel index, at least 0.
     * @param count The number of channels, at least 1.
     * @return The selection first, first + 1, ... first + count - 1.
     * @throws InvalidArgumentError When first < 0 or count < 1.
     */
    static ChannelSelection range(int32_t first, int32_t count);
    /**
     * @brief Builds a selection of every channel.
     * @return A selection with isAll() true and count() 0.
     * @throws std::bad_alloc When memory is exhausted.
     */
    static ChannelSelection all();

    /**
     * @brief Appends a channel.
     * @param channel The channel index, at least 0.
     * @throws InvalidArgumentError When channel is negative, already selected,
     * or when isAll() is true. The selection is unchanged on failure.
     * @throws std::bad_alloc When memory is exhausted; the selection is unchanged.
     */
    void add(int32_t channel);
    /**
     * @brief Tells whether every channel is selected.
     * @return True for a selection built by all().
     * @throws Nothing.
     */
    bool isAll() const noexcept;
    /**
     * @brief Returns the number of explicitly listed channels.
     * @return The count; 0 for all() and for an empty selection.
     * @throws Nothing.
     */
    int32_t count() const noexcept;
    /**
     * @brief Returns a listed channel.
     * @param index Position, from 0 to count() - 1, in insertion order.
     * @return The channel index.
     * @throws InvalidArgumentError When index is out of range, or isAll() is true.
     */
    int32_t at(int32_t index) const;
    /**
     * @brief Tests whether a channel is selected.
     * @param channel The channel index.
     * @return For all(), true for every channel >= 0; otherwise true when the
     * channel is listed. A negative channel is never contained.
     * @throws Nothing.
     */
    bool contains(int32_t channel) const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
