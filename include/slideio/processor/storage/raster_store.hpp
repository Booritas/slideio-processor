// Requirements: UR-RES-7, UR-STG-4, UR-STG-6, UR-STG-9, UR-STG-10
#pragma once

/**
 * @file raster_store.hpp
 * @brief RasterCompression, RasterDeclaration, RasterWriter and RasterRegistry: the
 * rasters a store declares, and the pyramidal TIFF files that hold their pixels.
 */

#include <slideio/processor/core/data_type.hpp>
#include <slideio/processor/core/export.hpp>
#include <slideio/processor/core/flow.hpp>
#include <slideio/processor/core/ids.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

namespace detail {
class StoreImpl;
} // namespace detail

/** @brief How a raster's TIFF tiles are compressed. Persisted: never renumber. */
enum class RasterCompression : int32_t {
    None,    ///< Stored uncompressed.
    Lzw,     ///< LZW, with a predictor.
    Deflate  ///< Deflate (zlib level 6), with a predictor. The default.
};

/**
 * @brief What a raster is: its name, sample type, plane count, compression and
 * description.
 *
 * Declared before anything runs, like a layer (UR-STG-10): the dependency graph must
 * resolve before the first stage starts, and a reader declared ahead of its writer must
 * have something to name. The constructor and the setters never validate;
 * RasterRegistry::declare() does. A moved-from declaration may only be destroyed or
 * assigned to; its getters answer as a declaration with a null name.
 * @par Thread-safety
 * ThreadCompatible: const members may run concurrently; the setters and assignment
 * need exclusive access.
 * @par Requirements
 * UR-STG-10, UR-RES-7
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS RasterDeclaration {
public:
    /**
     * @brief Starts a declaration, with Deflate compression and an empty description.
     * @param name UTF-8 raster name; copied. Null is kept as null.
     * @param type The sample type of every plane.
     * @param planeCount The number of planes.
     * @throws std::bad_alloc When memory is exhausted.
     */
    RasterDeclaration(const char* name, DataType type, int32_t planeCount);
    /**
     * @brief Copies a declaration.
     * @param other The declaration to copy.
     * @throws std::bad_alloc When memory is exhausted.
     */
    RasterDeclaration(const RasterDeclaration& other);
    /**
     * @brief Moves a declaration; `other` may then only be destroyed or assigned to.
     * @param other The declaration to move from.
     * @throws Nothing.
     */
    RasterDeclaration(RasterDeclaration&& other) noexcept;
    /**
     * @brief Copies a declaration.
     * @param other The declaration to copy.
     * @return This declaration.
     * @throws std::bad_alloc When memory is exhausted; this declaration is then
     * unchanged.
     */
    RasterDeclaration& operator=(const RasterDeclaration& other);
    /**
     * @brief Moves a declaration; `other` may then only be destroyed or assigned to.
     * @param other The declaration to move from.
     * @return This declaration.
     * @throws Nothing.
     */
    RasterDeclaration& operator=(RasterDeclaration&& other) noexcept;
    /** @brief Releases the declaration. */
    ~RasterDeclaration();

    /**
     * @brief Sets the compression of the raster's tiles.
     * @param compression The compression; Deflate unless set.
     * @return This declaration.
     * @throws Nothing.
     */
    RasterDeclaration& compression(RasterCompression compression);
    /**
     * @brief Sets the free-text description of the raster.
     * @param text UTF-8 text; copied. Null reads as empty.
     * @return This declaration.
     * @throws std::bad_alloc When memory is exhausted.
     */
    RasterDeclaration& description(const char* text);

    /**
     * @brief Returns the name given to the constructor.
     * @return A null-terminated UTF-8 string valid while this declaration lives and is
     * not modified; null when the constructor was given null, or after a move.
     * @throws Nothing.
     */
    const char* name() const noexcept;
    /**
     * @brief Returns the sample type given to the constructor.
     * @return The type; DataType::Unknown after a move.
     * @throws Nothing.
     */
    DataType planeType() const noexcept;
    /**
     * @brief Returns the plane count given to the constructor.
     * @return The count; 0 after a move.
     * @throws Nothing.
     */
    int32_t planeCount() const noexcept;
    /**
     * @brief Returns the compression set by compression().
     * @return The compression; Deflate unless set, and after a move.
     * @throws Nothing.
     */
    RasterCompression compressionValue() const noexcept;
    /**
     * @brief Returns the description set by description().
     * @return A null-terminated UTF-8 string valid while this declaration lives and is
     * not modified; empty unless set, and after a move. Never null.
     * @throws Nothing.
     */
    const char* descriptionText() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class RasterWriter;
/** @brief Shared ownership of a RasterWriter. */
using RasterWriterPtr = std::shared_ptr<RasterWriter>;

/**
 * @brief Writes one raster on behalf of one stage, and publishes it on flush().
 *
 * Obtained from RasterRegistry::writerForStage().
 * @par Thread-safety
 * writeTile() is ThreadSafe; flush() requires exclusive access.
 * @par Requirements
 * UR-RES-7, UR-STG-4, UR-STG-6
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS RasterWriter : public RasterSink {
public:
    /** @brief Abandons any tiles not yet flushed; never publishes. */
    ~RasterWriter() override;
    RasterWriter(const RasterWriter&) = delete;             ///< Not copyable.
    RasterWriter& operator=(const RasterWriter&) = delete;  ///< Not copyable.

    /**
     * @brief Accumulates one tile at the run's analysis level.
     *
     * Tiles may arrive in any order and need not cover the raster; unwritten area reads
     * as zero.
     * @param tile The tile.
     * @throws InvalidArgumentError When the tile does not match the declaration, the
     * level of the first tile or, within one level pixel, the scale of the widest tile so
     * far; or when the writer was flushed.
     * @throws IoError When the store is closed, or when the spool cannot be written.
     */
    void writeTile(const BlockView& tile) override;
    /**
     * @brief Completes the raster: builds the pyramid down from the written level and
     * publishes it.
     *
     * Until this returns, the raster is not readable and RasterRegistry::isPopulated()
     * is false. The scale is final here: that of the widest tile, and of the tallest.
     * Terminal: a second call is a no-op. With no tile written it publishes nothing. When
     * it fails, the raster's previous state stands.
     * @throws InvalidArgumentError When a tile does not fit the final scale within one
     * level pixel, or when another stage published the raster meanwhile; nothing is then
     * published.
     * @throws IoError When the store is closed, or when the file cannot be written.
     */
    void flush() override;
    /**
     * @brief Returns the number of tiles accepted by writeTile().
     * @return The count.
     * @throws Nothing.
     */
    int64_t tilesWritten() const override;

    /**
     * @brief Returns the raster this writer writes.
     * @return The raster.
     * @throws Nothing.
     */
    RasterId raster() const noexcept;
    /**
     * @brief Returns the stage the raster is attributed to.
     * @return The stage.
     * @throws Nothing.
     */
    StageId stage() const noexcept;

private:
    friend class RasterRegistry;
    class Impl;
    explicit RasterWriter(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief The rasters of a store: their declarations, which stage populated each, and
 * the files that hold their pixels.
 *
 * A raster is not a layer, and a RasterId is not a LayerId: raster names have their own
 * namespace, so a layer and a raster may share a name. Rasters number from 1 upward, in
 * declaration order, and are never removed. The pixels live beside the store, in the
 * directory "<store path>-rasters" (directory()), one pyramidal TIFF per populated
 * raster; the store holds one row per raster naming its file. A store file copied
 * without that directory therefore reads its rasters as declared and unpopulated.
 *
 * Every member throws IoError on a closed store (the noexcept ones answer as documented
 * instead), and every mutating member also on a ReadOnly store. A declaration appends a
 * RasterDeclared event in the same transaction.
 * @par Thread-safety
 * ThreadSafe: every member may be called from any thread with no external locking.
 * Declarations are serialised with every other write.
 * @par Requirements
 * UR-RES-7, UR-STG-4, UR-STG-6, UR-STG-9, UR-STG-10
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS RasterRegistry {
public:
    /** @brief Releases the registry. */
    ~RasterRegistry();
    RasterRegistry(const RasterRegistry&) = delete;             ///< Not copyable.
    RasterRegistry& operator=(const RasterRegistry&) = delete;  ///< Not copyable.

    /**
     * @brief Declares a raster, unpopulated.
     *
     * One write transaction inserts the raster and appends RasterDeclared, which carries
     * the raster. The directory() is not created.
     * @param declaration The raster's name, type, plane count, compression and
     * description.
     * @return The new raster id; the first is RasterId(1).
     * @throws InvalidArgumentError When the name is null, empty or already declared as
     * a raster (compared exactly); when the type is DataType::Unknown or out of range;
     * when the plane count is outside 1..65535; or when the compression is not a
     * RasterCompression. Nothing is then written.
     * @throws IoError When the store is closed or read-only, or when SQLite fails.
     * @throws std::bad_alloc When memory is exhausted.
     */
    RasterId declare(const RasterDeclaration& declaration);

    /**
     * @brief Finds a raster by name; the comparison is exact, with no case folding.
     * @param name UTF-8 raster name.
     * @param out Receives the id when found; unchanged otherwise.
     * @return True when the raster exists; false for a null name, an unknown name, a
     * closed store or a SQLite failure.
     * @throws Nothing.
     */
    bool find(const char* name, RasterId& out) const noexcept;
    /**
     * @brief Returns the name of a raster.
     * @param raster The raster.
     * @return A null-terminated UTF-8 string, valid for as long as the store lives;
     * rasters are never removed or renamed.
     * @throws InvalidArgumentError When the raster is unknown.
     * @throws IoError When the store is closed, or when SQLite fails.
     * @throws std::bad_alloc When memory is exhausted.
     */
    const char* name(RasterId raster) const;
    /**
     * @brief Returns the declared sample type of a raster's planes.
     * @param raster The raster.
     * @return The type.
     * @throws InvalidArgumentError When the raster is unknown.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    DataType planeType(RasterId raster) const;
    /**
     * @brief Returns the declared number of planes of a raster.
     * @param raster The raster.
     * @return The count, 1..65535.
     * @throws InvalidArgumentError When the raster is unknown.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    int32_t planeCount(RasterId raster) const;
    /**
     * @brief Tells whether a raster is declared.
     * @param raster The raster.
     * @return True when declared; false for an unknown raster, a closed store or a
     * SQLite failure.
     * @throws Nothing.
     */
    bool exists(RasterId raster) const noexcept;

    /**
     * @brief Tells whether some stage has written and flushed the raster, and its file
     * is present.
     *
     * False until then. A raster that was declared and never written is legal and reads
     * as zero. A populated raster whose file is missing from directory() reads as
     * unpopulated.
     * @param raster The raster.
     * @return True when the row is populated and its file exists; false otherwise, and
     * for an unknown raster, a closed store or a SQLite failure.
     * @throws Nothing.
     */
    bool isPopulated(RasterId raster) const noexcept;
    /**
     * @brief Returns the stage that populated a raster.
     * @param raster A populated raster (isPopulated()).
     * @return The stage.
     * @throws InvalidArgumentError When the raster is unknown or not populated.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    StageId writerStage(RasterId raster) const;
    /**
     * @brief Returns the pyramid level a raster was written at.
     * @param raster A populated raster (isPopulated()).
     * @return The level of the tiles its writer accepted.
     * @throws InvalidArgumentError When the raster is unknown or not populated.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    int32_t writtenAtLevel(RasterId raster) const;
    /**
     * @brief Returns the size of a raster's file.
     * @param raster The raster.
     * @return The file's size in bytes when the raster is populated (isPopulated()); 0
     * otherwise, and for an unknown raster.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    int64_t bytesOnDisk(RasterId raster) const;

    /**
     * @brief Returns a writer that attributes the raster to `stage`.
     *
     * One at a time per raster: unlike objects, a raster has a single producing stage,
     * because two stages writing overlapping tiles of one image have no merge rule.
     * @param stage The writing stage.
     * @param raster The raster to write.
     * @return The writer.
     * @throws InvalidArgumentError When the stage or raster is unknown, the raster
     * already has a live writer, or another stage populated it.
     * @throws IoError When the store is closed or read-only, or when the spool cannot
     * be created.
     */
    RasterWriterPtr writerForStage(StageId stage, RasterId raster);

    /**
     * @brief Reads a raster one tile at a time, at whatever level and region the view
     * carries.
     *
     * The same interface a layer rasterises through, so a consumer cannot tell whether
     * the coarse product it reads was pixels or objects. An unpopulated raster reads as
     * zero.
     *
     * The source is bound to the file the raster's row names now: a later re-run or
     * discard does not move it, and it keeps that file open while it lives. Its
     * planeType() and planeCount() are the declared ones. Its fillTile() reads the
     * coarsest pyramid page no coarser than the view (page 0 when the view is finer
     * than the raster was written), sampled nearest at the view's pixel centres; pixels
     * outside the raster's extent read 0. Only the view's geometry counts, not its
     * level() number. fillTile() throws InvalidArgumentError unless the view has exactly
     * planeCount() planes of planeType() and a non-empty size and slideRect, and IoError
     * naming the file when it cannot be read. The source is ThreadSafe, needs nothing
     * of the store once made, and may outlive it.
     * @param raster The raster.
     * @return The source.
     * @throws InvalidArgumentError When the raster is unknown.
     * @throws IoError When the store is closed, or when the file cannot be opened or is
     * not the raster its row describes.
     */
    TileRasterSourcePtr sourceFor(RasterId raster) const;

    /**
     * @brief Removes the rasters a stage wrote: deletes their files and clears their
     * population state. The declarations survive, as a layer's does.
     * @param stage The stage.
     * @throws InvalidArgumentError When the stage is unknown.
     * @throws IoError When the store is closed or read-only, or when SQLite fails.
     * @throws InternalError Until discarding is implemented.
     */
    void discardRasterOfStage(StageId stage);

    /**
     * @brief Returns the number of declared rasters.
     * @return The count; 0 when the store is closed or SQLite fails.
     * @throws Nothing.
     */
    int32_t count() const noexcept;
    /**
     * @brief Returns the raster at a position in ascending id order.
     * @param index Zero-based position, below count().
     * @return The raster.
     * @throws InvalidArgumentError When index is negative or not below count().
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    RasterId at(int32_t index) const;
    /**
     * @brief Returns the total size of every populated raster's file.
     * @return The sum of bytesOnDisk() over every raster.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    int64_t totalBytesOnDisk() const;

    /**
     * @brief Returns the directory that holds the raster files.
     *
     * The store path with "-rasters" appended, beside the store as its "-wal" file is.
     * It is created by the first writerForStage(), never by declare().
     * @return A null-terminated UTF-8 path, valid for as long as the store lives, even
     * after close().
     * @throws Nothing.
     */
    const char* directory() const noexcept;

    /**
     * @brief Deletes the files in directory() that no raster refers to.
     *
     * Only names the library generates are considered (32 lowercase hex digits, then
     * ".tif" or ".spool"); every other file is left alone. A file named by a raster, or
     * owned by a live writer of this store, is kept. A file that cannot be deleted is
     * logged at Warning and not counted.
     * @return The bytes reclaimed; 0 when directory() does not exist.
     * @throws IoError When the store is closed or read-only.
     * @throws InternalError Until the sweep is implemented.
     */
    int64_t removeUnreferencedFiles();

private:
    friend class Store;
    explicit RasterRegistry(detail::StoreImpl& store);
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
