// Requirements: UR-IMG-12, UR-ALG-4, UR-ALG-6, UR-ALG-8, UR-EXT-5
#pragma once

/**
 * @file operation_catalog.hpp
 * @brief OperationFactory and OperationCatalog: the registry that resolves
 * operations by name, and the support matrix generated from it.
 */

#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/pipeline/export.hpp>
#include <slideio/processor/pipeline/operation.hpp>
#include <slideio/processor/pipeline/operation_parameters.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

/**
 * @brief Creates one kind of operation and declares what it is, so the catalog
 * can answer before constructing anything.
 *
 * name(), arity(), requiresUniformTypes(), supportedTypes() and
 * producesObjects() do not depend on parameters and must equal those of every
 * operation create() returns; the catalog checks them on each create(). The
 * other declarations (halo(), tileRequirement(),
 * workingSetBytesPerLevelPixel(), outputPlaneCount(),
 * workingSetBytesPerTile()) describe the operation
 * created from default parameters; the created operation's own declarations
 * are authoritative.
 * @par Thread-safety
 * Implementations must be safe to use concurrently: the catalog calls them from
 * any thread.
 * @par Requirements
 * UR-EXT-5, UR-ALG-4, UR-ALG-6
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS OperationFactory {
public:
    /**
     * @brief Destroys the factory.
     * @throws Nothing.
     */
    virtual ~OperationFactory();

    /**
     * @brief Builds an operation from its parameters.
     * @param parameters The parameters; see Operation::parameters().
     * @return The operation, never null.
     * @throws InvalidArgumentError When the parameters are not acceptable.
     * @par Thread-safety
     * Must be safe to call concurrently.
     * @par Requirements
     * UR-EXT-5
     */
    virtual OperationPtr create(const OperationParameters& parameters) const = 0;
    /**
     * @brief Returns the name the catalog registers the factory under.
     * @return A non-empty UTF-8 name, valid for the life of the factory.
     * @par Thread-safety
     * Must be safe to call concurrently.
     * @par Requirements
     * UR-EXT-5
     */
    virtual const char*  name() const = 0;
    /**
     * @brief Returns Operation::arity() of the operations created.
     * @return The arity.
     * @par Thread-safety
     * Must be safe to call concurrently.
     * @par Requirements
     * UR-ALG-3
     */
    virtual ChannelArity arity() const = 0;
    /**
     * @brief Returns Operation::requiresUniformTypes() of the operations created.
     * @return The flag.
     * @par Thread-safety
     * Must be safe to call concurrently.
     * @par Requirements
     * UR-ALG-3
     */
    virtual bool         requiresUniformTypes() const = 0;
    /**
     * @brief Returns Operation::supportedTypes() of the operations created.
     * @return The set.
     * @par Thread-safety
     * Must be safe to call concurrently.
     * @par Requirements
     * UR-IMG-12, UR-ALG-4
     */
    virtual DataTypeSet  supportedTypes() const = 0;
    /**
     * @brief Returns Operation::halo() of an operation created from default parameters; the instance is authoritative for its own parameters.
     * @return The halo.
     * @par Thread-safety
     * Must be safe to call concurrently.
     * @par Requirements
     * UR-EXT-3
     */
    virtual Halo         halo() const = 0;
    /**
     * @brief Names what the operation's results are checked against.
     * @return A non-empty UTF-8 text: an analytic result, a published
     * implementation or a peer-reviewed definition (UR-ALG-6).
     * @par Thread-safety
     * Must be safe to call concurrently.
     * @par Requirements
     * UR-ALG-6
     */
    virtual const char*  correctnessReference() const = 0;
    /**
     * @brief States the operation's cost in the size of its input.
     * @return A UTF-8 text, never null; may be empty.
     * @par Thread-safety
     * Must be safe to call concurrently.
     * @par Requirements
     * UR-ALG-8
     */
    virtual const char*  complexityNote() const = 0;

    /**
     * @brief Returns Operation::tileRequirement() for default parameters; the instance is authoritative.
     * @return TileRequirement::Halo by default.
     * @par Thread-safety
     * Must be safe to call concurrently.
     * @par Requirements
     * UR-EXT-3, UR-EXT-4
     */
    virtual TileRequirement tileRequirement() const;
    /**
     * @brief Returns, for default parameters (the instance is authoritative), Operation::workingSetBytesPerLevelPixel() of the
     * operations created.
     * @return 0 by default.
     * @par Thread-safety
     * Must be safe to call concurrently.
     * @par Requirements
     * UR-EXT-4
     */
    virtual int64_t workingSetBytesPerLevelPixel() const;
    /**
     * @brief Returns Operation::producesObjects() of the operations created.
     * @return False by default.
     * @par Thread-safety
     * Must be safe to call concurrently.
     * @par Requirements
     * UR-EXT-4
     */
    virtual bool producesObjects() const;
    /**
     * @brief Tells whether the operations created give the same result on every run over the same input.
     *
     * An operation whose results may vary between runs (a GPU model, for example) overrides this to return false;
     * stage records list such operations (SDS 04).
     * @return True by default.
     * @par Thread-safety
     * Must be safe to call concurrently.
     * @par Requirements
     * UR-EXT-4
     */
    virtual bool isDeterministic() const;
    /**
     * @brief Tells where the operation's code comes from, for provenance; empty for the library's own operations.
     *
     * Stage records list the non-empty origins of their operations (SDS 04).
     * @return The origin text; empty by default; never null.
     * @par Thread-safety
     * Must be safe to call concurrently.
     * @par Requirements
     * UR-EXT-4
     */
    virtual const char* origin() const;
    /**
     * @brief Returns, for default parameters (the instance is authoritative), Operation::outputPlaneCount() of the operations created.
     * @param inputPlaneCount Number of input planes; at least 1.
     * @return @p inputPlaneCount by default.
     * @throws InvalidArgumentError What Operation::outputPlaneCount() throws.
     * @par Thread-safety
     * Must be safe to call concurrently.
     * @par Requirements
     * UR-ALG-3
     */
    virtual int32_t outputPlaneCount(int32_t inputPlaneCount) const;
    /**
     * @brief Returns, for default parameters (the instance is authoritative), Operation::workingSetBytesPerTile() of the
     * operations created.
     * @return 0 by default.
     * @par Thread-safety
     * Must be safe to call concurrently.
     * @par Requirements
     * UR-EXE-2, UR-EXT-4
     */
    virtual int64_t workingSetBytesPerTile() const;
    /**
     * @brief Returns, for default parameters (the instance is authoritative), Operation::accumulatedStateBytes() of the
     * operations created.
     * @param planeCount Number of planes the operation's input step has.
     * @return 0 by default.
     * @par Thread-safety
     * Must be safe to call concurrently.
     * @par Requirements
     * UR-EXE-2, UR-EXT-4
     */
    virtual int64_t accumulatedStateBytes(int32_t planeCount) const;
};
/** @brief Shared ownership of a factory, as the catalog holds it. */
using OperationFactoryPtr = std::shared_ptr<OperationFactory>;

/**
 * @brief The process-wide registry of operations, by name.
 *
 * What keeps `algorithm` a leaf: stages are resolved against the catalog by
 * name. A user-defined operation registers the same way a built-in does
 * (UR-EXT-5). Names are kept in ascending bytewise order. A factory is never
 * removed, so a pointer returned by a query stays valid for the life of the
 * process.
 * @par Thread-safety
 * ThreadSafe.
 * @par Requirements
 * UR-IMG-12, UR-ALG-4, UR-ALG-6, UR-ALG-8, UR-EXT-5
 */
class SLIDEIO_PROCESSOR_ENGINE_EXPORTS OperationCatalog {
public:
    /**
     * @brief Returns the one catalog.
     * @return A reference valid for the life of the process.
     * @throws Nothing. Declared noexcept although the first call allocates:
     * failure to allocate is fatal and calls std::terminate.
     * @par Thread-safety
     * Safe to call concurrently.
     * @par Requirements
     * UR-EXT-5
     */
    static OperationCatalog& instance() noexcept;

    /**
     * @brief Registers a factory under its name().
     * @param factory The factory; the catalog keeps it.
     * @throws InvalidArgumentError When @p factory is null, its name() is null
     * or empty, its correctnessReference() is null or empty, its
     * complexityNote() is null, or the name is already registered (the message
     * names it).
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-EXT-5, UR-ALG-6
     */
    void registerFactory(OperationFactoryPtr factory);
    /**
     * @brief Tells whether a name is registered.
     * @param name The UTF-8 name; null is never registered.
     * @return True when registered.
     * @throws Nothing. Declared noexcept although it takes the catalog's
     * shared lock: failure to lock (std::system_error) is fatal and calls
     * std::terminate. The lookup itself does not allocate.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-EXT-5
     */
    bool isRegistered(const char* name) const noexcept;

    /**
     * @brief Builds an operation by name.
     * @param name The UTF-8 name.
     * @param parameters The operation's parameters.
     * @return The operation, never null.
     * @throws InvalidArgumentError When the name is null or unknown (the
     * message names it), or the factory rejects the parameters.
     * @throws InternalError When the factory returns null, or the operation's
     * name(), arity(), requiresUniformTypes(), supportedTypes() or
     * producesObjects() differs from the factory's (the
     * message names the field); also for a non-ProcessorError exception the
     * factory throws, kept as cause(), or anything else it throws. halo(), tileRequirement(), the working
     * set and outputPlaneCount() are not compared: they may depend on the
     * parameters, which a factory does not see.
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-EXT-5
     */
    OperationPtr create(const char* name, const OperationParameters& parameters) const;

    /**
     * @brief Returns the number of registered operations.
     * @return The count.
     * @throws Nothing. Declared noexcept although it takes the catalog's
     * shared lock: failure to lock (std::system_error) is fatal and calls
     * std::terminate.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-EXT-5
     */
    int32_t     count() const noexcept;
    /**
     * @brief Returns a registered name by position, in ascending bytewise order.
     * @param index From 0 to count() - 1.
     * @return The name, valid for the life of the process.
     * @throws InvalidArgumentError When @p index is out of range.
     * @par Thread-safety
     * ThreadSafe. Positions shift when a smaller name is registered later.
     * @par Requirements
     * UR-EXT-5
     */
    const char* nameAt(int32_t index) const;

    /**
     * @brief Returns OperationFactory::arity() of a registered operation.
     * @param name The operation name.
     * @return The arity.
     * @throws InvalidArgumentError When the name is null or unknown.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-ALG-3
     */
    ChannelArity arity(const char* name) const;
    /**
     * @brief Returns OperationFactory::supportedTypes() of a registered operation.
     * @param name The operation name.
     * @return The declared set; the backend may compute fewer (see writeSupportMatrix()).
     * @throws InvalidArgumentError When the name is null or unknown.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-IMG-12, UR-ALG-4
     */
    DataTypeSet  supportedTypes(const char* name) const;
    /**
     * @brief Returns OperationFactory::halo() of a registered operation, for default parameters.
     * @param name The operation name.
     * @return The halo.
     * @throws InvalidArgumentError When the name is null or unknown.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-EXT-3
     */
    Halo         halo(const char* name) const;
    /**
     * @brief Returns OperationFactory::correctnessReference() of a registered operation.
     * @param name The operation name.
     * @return The reference text, valid for the life of the process.
     * @throws InvalidArgumentError When the name is null or unknown.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-ALG-6
     */
    const char*  correctnessReference(const char* name) const;
    /**
     * @brief Returns OperationFactory::complexityNote() of a registered operation.
     * @param name The operation name.
     * @return The note, valid for the life of the process.
     * @throws InvalidArgumentError When the name is null or unknown.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-ALG-8
     */
    const char*  complexityNote(const char* name) const;

    /**
     * @brief Returns, for default parameters, OperationFactory::tileRequirement() of a registered
     * operation.
     * @param name The operation name.
     * @return The requirement.
     * @throws InvalidArgumentError When the name is null or unknown.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-EXT-4
     */
    TileRequirement tileRequirement(const char* name) const;
    /**
     * @brief Returns, for default parameters, OperationFactory::workingSetBytesPerLevelPixel() of a
     * registered operation.
     * @param name The operation name.
     * @return Bytes per level pixel.
     * @throws InvalidArgumentError When the name is null or unknown.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-EXT-4
     */
    int64_t workingSetBytesPerLevelPixel(const char* name) const;
    /**
     * @brief Returns OperationFactory::producesObjects() of a registered
     * operation.
     * @param name The operation name.
     * @return True when it emits objects.
     * @throws InvalidArgumentError When the name is null or unknown.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-EXT-4
     */
    bool producesObjects(const char* name) const;
    /**
     * @brief Returns OperationFactory::isDeterministic() of a registered
     * operation.
     * @param name The operation name.
     * @return False when the operation's results may vary between runs.
     * @throws InvalidArgumentError When the name is null or unknown.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-EXT-4
     */
    bool isDeterministic(const char* name) const;
    /**
     * @brief Returns OperationFactory::origin() of a registered operation.
     * @param name The operation name.
     * @return The origin text; empty for the library's own operations.
     * @throws InvalidArgumentError When the name is null or unknown.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-EXT-4
     */
    const char* origin(const char* name) const;
    /**
     * @brief Returns, for default parameters, OperationFactory::outputPlaneCount() of a registered
     * operation.
     * @param name The operation name.
     * @param inputPlaneCount Number of input planes; at least 1.
     * @return The number of output planes of a Joint operation.
     * @throws InvalidArgumentError When the name is null or unknown, or
     * what OperationFactory::outputPlaneCount() throws.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-ALG-3
     */
    int32_t outputPlaneCount(const char* name, int32_t inputPlaneCount) const;
    /**
     * @brief Returns, for default parameters, OperationFactory::workingSetBytesPerTile() of a
     * registered operation.
     * @param name The operation name.
     * @return Bytes per apply() or accumulate() call.
     * @throws InvalidArgumentError When the name is null or unknown.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-EXE-2, UR-EXT-4
     */
    int64_t workingSetBytesPerTile(const char* name) const;
    /**
     * @brief Returns, for default parameters, OperationFactory::accumulatedStateBytes() of a
     * registered operation.
     * @param name The operation name.
     * @param planeCount Number of planes the operation's input step has.
     * @return Bytes per instance.
     * @throws InvalidArgumentError When the name is null or unknown.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-EXE-2, UR-EXT-4
     */
    int64_t accumulatedStateBytes(const char* name, int32_t planeCount) const;

    /**
     * @brief Emits the operation-by-type support matrix as CSV, generated from
     * the same declarations the runtime enforces.
     *
     * UTF-8, "\n" line endings. The header is
     * `operation,Byte,Int8,UInt16,Int16,UInt32,Int32,UInt64,Int64,Float16,Float32,Float64`;
     * one row per operation in nameAt() order; a cell is `yes` when the
     * operation declares the type and the backend computes it, else `no`.
     * @param path UTF-8 path of the file to write; replaced if it exists.
     * @return The number of bytes written.
     * @throws InvalidArgumentError When @p path is null or empty.
     * @throws IoError When the file cannot be opened or written (the message
     * names the file by its role and its path).
     * @throws std::bad_alloc When memory is exhausted.
     * @par Thread-safety
     * ThreadSafe.
     * @par Requirements
     * UR-IMG-12
     */
    int64_t writeSupportMatrix(const char* path) const;

private:
    OperationCatalog();
    ~OperationCatalog();
    OperationCatalog(const OperationCatalog&) = delete;
    OperationCatalog& operator=(const OperationCatalog&) = delete;

    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
