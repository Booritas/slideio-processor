// Requirements: UR-STG-4, UR-RES-11, UR-EXE-1, UR-IMG-5
#pragma once

/**
 * @file pipeline_spec.hpp
 * @brief PipelineSpec: a pipeline run described as a value, with a replayable JSON record.
 */

#include <slideio/processor/core/channel_selection.hpp>
#include <slideio/processor/core/geometry.hpp>
#include <slideio/processor/pipeline/export.hpp>
#include <slideio/processor/pipeline/operation.hpp>
#include <slideio/processor/pipeline/operation_parameters.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

/**
 * @brief What to run, separated from the running of it: channels, an optional
 * region, an analysis level and an ordered list of operations named by catalog
 * name with their parameters.
 *
 * A spec holds no scene, store, budget or thread, and resolves nothing; an
 * unknown operation name fails at Pipeline::validate(), not at append. That
 * makes it comparable (equals() gives staleness a definition that does not
 * depend on key order or float formatting) and recordable: toJson() and
 * fromJson() are a real round trip, so a stage configuration read out of a store
 * replays without the code that wrote it.
 *
 * The JSON form, schema version 1, is one object:
 * `{"specVersion":1,"channels":"all"|[c,...],"analysisLevel":L,
 * "roi":[x,y,w,h],"polygon":[{"hole":bool,"points":[[x,y],...]},...],
 * "operations":[{"name":"...","parameters":{...}},...]}`. "roi" and "polygon"
 * are present only when set. Coordinates are JSON integers.
 * @par Thread-safety
 * ThreadConfined to build; Immutable in effect once handed to a Pipeline,
 * which copies it. Const members are safe to call concurrently.
 * @par Requirements
 * UR-STG-4, UR-RES-11, UR-EXE-1, UR-IMG-5
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS PipelineSpec {
public:
    /**
     * @brief Creates a spec with all channels, automatic level, no restriction and no operations.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-STG-4
     */
    PipelineSpec();
    /**
     * @brief Copies a spec; the copy shares nothing with the original.
     * @param other The spec to copy.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Reads @p other; ThreadCompatible.
     * @par Requirements
     * UR-STG-4
     */
    PipelineSpec(const PipelineSpec& other);
    /**
     * @brief Moves a spec. The moved-from spec is reset to the default state and reusable.
     * @param other The spec to move from.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-STG-4
     */
    PipelineSpec(PipelineSpec&& other) noexcept;
    /**
     * @brief Replaces this spec with a copy of another.
     * @param other The spec to copy.
     * @return This spec.
     * @throws std::bad_alloc When allocation fails; this spec is then unchanged.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-STG-4
     */
    PipelineSpec& operator=(const PipelineSpec& other);
    /**
     * @brief Replaces this spec by moving another.
     * @param other The spec to move from; reset to the default state.
     * @return This spec.
     * @throws Nothing.
     * @par Thread-safety
     * ThreadCompatible.
     * @par Requirements
     * UR-STG-4
     */
    PipelineSpec& operator=(PipelineSpec&& other) noexcept;
    ~PipelineSpec();

    /**
     * @brief Sets the channels to read; replaces any earlier selection.
     * @param selection The channels.
     * @return This spec, for chaining.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-4, UR-EXE-1
     */
    PipelineSpec& channels(const ChannelSelection& selection);
    /**
     * @brief Restricts the run to a level-0 rectangle; replaces any earlier restriction of this kind.
     * @param roi The rectangle, in level-0 pixels.
     * @return This spec, for chaining.
     * @throws InvalidArgumentError When the rectangle is empty.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-IMG-5
     */
    PipelineSpec& restrictTo(const Rect64& roi);
    /**
     * @brief Restricts the run to a polygon in level-0 pixels; replaces any earlier polygon.
     * @param polygon The region.
     * @return This spec, for chaining.
     * @throws InvalidArgumentError When the polygon is empty.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-IMG-5
     */
    PipelineSpec& restrictToPolygon(const Polygon& polygon);

    /**
     * @brief The pyramid level to read from; -1 (the default) lets `LevelSelector`
     * choose the coarsest adequate one (UR-IMG-4). A property of the run,
     * not of any one operation: the tile grid is laid out at this level
     * (5.6), so a chain cannot mix levels.
     * @param level The level, or -1 for automatic.
     * @return This spec, for chaining.
     * @throws InvalidArgumentError When level is below -1.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-EXE-1
     */
    PipelineSpec& analysisLevel(int32_t level);

    /**
     * @brief Appends an operation by catalog name. Nothing is resolved here; an
     * unknown name fails at Pipeline::validate(), not at append.
     * @param name UTF-8 catalog name, non-null and non-empty.
     * @param parameters The parameters of the operation; copied.
     * @return This spec, for chaining.
     * @throws InvalidArgumentError When the name is null or empty.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-4
     */
    PipelineSpec& operation(const char* name, const OperationParameters& parameters);

    /**
     * @brief Appends an already-constructed operation, lowered to its name and
     * parameters. The typed factories of `algorithm` return `OperationPtr`,
     * and this is how a C++ caller who used them builds a spec.
     * @param operation The operation; only its name() and parameters() are kept.
     * @return This spec, for chaining.
     * @throws InvalidArgumentError When the pointer is null or the name is null or empty.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * ThreadConfined.
     * @par Requirements
     * UR-STG-4
     */
    PipelineSpec& then(const OperationPtr& operation);

    /**
     * @brief Returns the number of operations.
     * @return The count.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently on a const spec.
     * @par Requirements
     * UR-STG-4
     */
    int32_t operationCount() const noexcept;
    /**
     * @brief Returns the catalog name of an operation.
     * @param index Position, from 0 to operationCount() - 1.
     * @return The name; valid until the spec is modified or destroyed.
     * @throws InvalidArgumentError When index is out of range.
     * @par Thread-safety
     * Safe to call concurrently on a const spec.
     * @par Requirements
     * UR-STG-4
     */
    const char* operationName(int32_t index) const;
    /**
     * @brief Returns the parameters of an operation.
     * @param index Position, from 0 to operationCount() - 1.
     * @return The parameters; valid until the spec is modified or destroyed.
     * @throws InvalidArgumentError When index is out of range.
     * @par Thread-safety
     * Safe to call concurrently on a const spec.
     * @par Requirements
     * UR-STG-4
     */
    const OperationParameters& operationParameters(int32_t index) const;

    /**
     * @brief Returns the channel selection.
     * @return The selection; all channels by default.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently on a const spec.
     * @par Requirements
     * UR-EXE-1
     */
    const ChannelSelection& channels() const noexcept;
    /**
     * @brief Returns the analysis level.
     * @return The level, or -1 for automatic.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently on a const spec.
     * @par Requirements
     * UR-EXE-1
     */
    int32_t analysisLevel() const noexcept;
    /**
     * @brief Tells whether a rectangle restriction is set.
     * @return True after restrictTo().
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently on a const spec.
     * @par Requirements
     * UR-IMG-5
     */
    bool hasRoi() const noexcept;
    /**
     * @brief Returns the rectangle restriction.
     * @return The rectangle, or an empty one when none is set.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently on a const spec.
     * @par Requirements
     * UR-IMG-5
     */
    Rect64 roi() const noexcept;
    /**
     * @brief Tells whether a polygon restriction is set.
     * @return True after restrictToPolygon().
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently on a const spec.
     * @par Requirements
     * UR-IMG-5
     */
    bool hasPolygon() const noexcept;
    /**
     * @brief Returns the polygon restriction.
     * @return The polygon, or an empty one when none is set; valid until the spec is modified or destroyed.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently on a const spec.
     * @par Requirements
     * UR-IMG-5
     */
    const Polygon& polygon() const noexcept;

    /**
     * @brief The recorded form. fromJson() reconstructs a spec from it, so a stage
     * configuration read out of a store replays without the code that wrote
     * it (UR-RES-11).
     * @param buffer Caller buffer for UTF-8 text with a terminator; null queries the length.
     * @param capacity Size of @p buffer in bytes; the text is truncated to fit.
     * @return The length of the full text, excluding the terminator.
     * @throws InternalError When serialisation fails.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Safe to call concurrently on a const spec.
     * @par Requirements
     * UR-STG-4, UR-RES-11
     */
    int32_t toJson(char* buffer, int32_t capacity) const;
    /**
     * @brief Reconstructs a spec from its recorded form.
     * @param json UTF-8 JSON text produced by toJson().
     * @return The spec.
     * @throws InvalidArgumentError When the text is null, does not parse (the message
     * carries the offset), lacks a member, holds a member of the wrong type or value
     * (the message names it), or has a specVersion other than 1.
     * @throws std::bad_alloc When allocation fails.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-STG-4, UR-RES-11
     */
    static PipelineSpec fromJson(const char* json);

    /**
     * @brief Compares two specs by value: channels (all versus an explicit list,
     * element-wise), level, restriction presence and value (a polygon ring by ring,
     * hole flags included), and operations by name and parameters.
     * @param other The spec to compare with.
     * @return True when every part is equal.
     * @throws Nothing.
     * @par Thread-safety
     * Safe to call concurrently on const specs.
     * @par Requirements
     * UR-STG-4
     */
    bool equals(const PipelineSpec& other) const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
