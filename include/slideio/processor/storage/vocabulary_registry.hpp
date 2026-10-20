// Requirements: UR-RES-17, UR-RES-18, UR-RES-19
#pragma once

/**
 * @file vocabulary_registry.hpp
 * @brief VocabularyRegistry: the labels a store holds and the import policy for unknown ones.
 */

#include <slideio/processor/core/export.hpp>
#include <slideio/processor/core/ids.hpp>
#include <slideio/processor/storage/types.hpp>

#include <cstdint>
#include <memory>

namespace slideio::processor {

namespace detail {
class StoreImpl;
} // namespace detail

/**
 * @brief The label vocabulary of a store: the standard labels it was created with,
 * those a newer standard version added, and the workspace-local ones.
 *
 * A store is created holding the ten labels of the standard vocabulary, ids 1 to 10.
 * Ids 1 to 65535 are reserved for the standard vocabulary, so a later standard
 * version can append ids without colliding with a local id already written;
 * workspace-local labels number from 65536 upward. Labels are never removed or
 * renamed, so ids, names and positions are stable. The store's own table is
 * authoritative for every name: a label of a newer standard version keeps the
 * identifier and string it was written with (UR-RES-17).
 *
 * Lookup folds ASCII case, then consults a fixed table of QuPath spellings
 * (`tumor`, `artifact`, `immune cells`, `ignore*` resolve to `tumour`, `artefact`,
 * `immune`, `ignore`); the `*` is an ordinary character, not a wildcard (UR-RES-18).
 * name() always returns the store's canonical spelling.
 *
 * Every member throws IoError on a closed store (the noexcept ones answer as
 * documented instead), and every mutating member also on a ReadOnly store. A new
 * local label appends a LabelAdded event in the same transaction, carrying the label
 * id with an empty bounding box and a count of 0.
 * @par Thread-safety
 * ThreadSafe: every member may be called from any thread with no external locking.
 * Additions are serialised with every other write.
 * @par Requirements
 * UR-RES-17, UR-RES-18, UR-RES-19
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS VocabularyRegistry {
public:
    /** @brief Releases the registry. */
    ~VocabularyRegistry();
    VocabularyRegistry(const VocabularyRegistry&) = delete;             ///< Not copyable.
    VocabularyRegistry& operator=(const VocabularyRegistry&) = delete;  ///< Not copyable.

    /**
     * @brief Returns the standard vocabulary version this store was written against.
     *
     * Fixed at creation; it may be older or newer than the linked library's.
     * @return The version; 0 when the store is closed or SQLite fails.
     * @throws Nothing.
     */
    int32_t recordedStandardVersion() const noexcept;

    /**
     * @brief Adds a workspace-local label.
     *
     * Legal at any time, including after stages have completed, and never requires a
     * format migration. When find() already resolves the name (case folded, aliases
     * included) nothing is added and no event is appended.
     * @param name UTF-8 label name; stored as given.
     * @return The id of the existing label, or of the new one (origin WorkspaceLocal,
     * numbered from 65536).
     * @throws InvalidArgumentError When name is null or empty.
     * @throws IoError When the store is closed or read-only, or when SQLite fails.
     * @throws std::bad_alloc When memory is exhausted.
     */
    LabelId addLocal(const char* name);

    /**
     * @brief Finds a label by name, folding ASCII case and applying the QuPath aliases.
     * @param name UTF-8 label name; may be null.
     * @param out Receives the id when found; unchanged otherwise.
     * @return True when the label exists; false for a null or empty name, an unknown
     * name, a closed store or a SQLite failure.
     * @throws Nothing.
     */
    bool find(const char* name, LabelId& out) const noexcept;
    /**
     * @brief Returns the name of a label.
     * @param label The label.
     * @return The store's canonical spelling, a null-terminated UTF-8 string valid for
     * as long as this registry lives; labels are never removed or renamed.
     * @throws InvalidArgumentError When the label is unknown.
     * @throws IoError When the store is closed, or when SQLite fails.
     * @throws std::bad_alloc When memory is exhausted.
     */
    const char* name(LabelId label) const;
    /**
     * @brief Returns where a label came from.
     * @param label The label.
     * @return Standard, WorkspaceLocal or ImportRegistered.
     * @throws InvalidArgumentError When the label is unknown.
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    LabelOrigin origin(LabelId label) const;

    /**
     * @brief Tells whether the linked library knows a label.
     *
     * False for a label from a newer standard vocabulary than this library knows (an
     * id in 1 to 65535 above StandardVocabulary::labelCount()); its identifier and
     * string are still returned intact. Not an error.
     * @param label The label.
     * @return True for a held standard label the library knows and for a held local
     * label; false otherwise, including for an unknown id, a closed store or a SQLite
     * failure.
     * @throws Nothing.
     */
    bool isRecognised(LabelId label) const noexcept;

    /**
     * @brief Returns the number of labels held.
     * @return The count; 10 in a new store. 0 when the store is closed or SQLite fails.
     * @throws Nothing.
     */
    int32_t count() const noexcept;
    /**
     * @brief Returns the label at a position in ascending id order (standard labels,
     * then local ones).
     * @param index Zero-based position, below count().
     * @return The label id.
     * @throws InvalidArgumentError When index is negative or not below count().
     * @throws IoError When the store is closed, or when SQLite fails.
     */
    LabelId at(int32_t index) const;

    /**
     * @brief Returns how a GeoJSON import treats labels the vocabulary lacks.
     *
     * Read from the store on every call, so a change made through another handle is
     * seen.
     * @return The policy; Strict in a new store, and when the store is closed or
     * SQLite fails.
     * @throws Nothing.
     */
    ImportPolicy importPolicy() const noexcept;
    /**
     * @brief Sets the import policy; it persists in the store.
     * @param policy The policy.
     * @throws InvalidArgumentError When policy is not an ImportPolicy.
     * @throws IoError When the store is closed or read-only, or when SQLite fails.
     */
    void setImportPolicy(ImportPolicy policy);

private:
    friend class Store;
    explicit VocabularyRegistry(detail::StoreImpl& store);
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace slideio::processor
