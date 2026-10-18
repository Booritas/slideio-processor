// Requirements: UR-RES-17
#pragma once

/**
 * @file standard_vocabulary.hpp
 * @brief StandardVocabulary: the shipped, versioned label vocabulary.
 */

#include <slideio/processor/core/export.hpp>
#include <slideio/processor/core/ids.hpp>

#include <cstdint>

namespace slideio::processor {

/**
 * @brief The label vocabulary that ships with the library (version 1).
 *
 * Compiled in, immutable, and versioned independently of the store format. It
 * is global to a store: one set of labels shared by every layer, never scoped
 * or subsetted, so nothing here rejects a label because of the layer it is
 * applied to.
 *
 * Label ids are a persistence contract: 1 to 10, in table order. Names are
 * matched exactly and case-sensitively.
 *
 * | Id | Label | conventionalLayer | Definition |
 * |---|---|---|---|
 * | 1 | tumour | region or cell | Neoplastic tissue, or a neoplastic cell |
 * | 2 | stroma | region or cell | Supporting connective tissue, non-neoplastic |
 * | 3 | necrosis | region | Non-viable tissue |
 * | 4 | immune | cell | An immune cell of unspecified lineage |
 * | 5 | lymphocyte | cell | A lymphocyte specifically |
 * | 6 | positive | any | Marker-positive by the asserting stage's criterion |
 * | 7 | negative | any | Marker-negative by the asserting stage's criterion |
 * | 8 | artefact | region | Fold, bubble, pen mark, out of focus: present, not analysable |
 * | 9 | ignore | region | Deliberately excluded from analysis |
 * | 10 | other | any | Classified, and none of the above |
 *
 * @par Thread-safety
 * Immutable.
 * @par Requirements
 * UR-RES-17
 */
class SLIDEIO_PROCESSOR_DATA_EXPORTS StandardVocabulary {
public:
    /**
     * @brief Returns the one shipped vocabulary.
     * @return A reference valid for the life of the process.
     * @throws Nothing.
     */
    static const StandardVocabulary& instance() noexcept;

    /**
     * @brief Returns the vocabulary version.
     * @return 1.
     * @throws Nothing.
     */
    int32_t version() const noexcept;
    /**
     * @brief Returns the number of standard labels.
     * @return 10 in version 1.
     * @throws Nothing.
     */
    int32_t labelCount() const noexcept;
    /**
     * @brief Returns the id of the label at a table position.
     * @param index A position in [0, labelCount()).
     * @return LabelId(index + 1).
     * @throws InvalidArgumentError When index is outside [0, labelCount()).
     */
    LabelId labelAt(int32_t index) const;
    /**
     * @brief Returns the name of a standard label.
     * @param id The label.
     * @return The name, or "" when id is not standard. Valid for the life of the process.
     * @throws Nothing.
     */
    const char* labelName(LabelId id) const;
    /**
     * @brief Looks a label up by name.
     * @param name The name; matched exactly and case-sensitively. May be null.
     * @param out Receives the id when found; left unchanged otherwise.
     * @return True when name is a standard label; false for a null, empty or unknown name.
     * @throws Nothing.
     */
    bool findLabel(const char* name, LabelId& out) const noexcept;
    /**
     * @brief Tells whether an id is a standard label.
     * @param id The id to test.
     * @return True for ids 1 to 10.
     * @throws Nothing.
     */
    bool isStandard(LabelId id) const noexcept;

    /**
     * @brief Returns the layer kind a label is conventionally used with.
     * @param id The label.
     * @return "region or cell", "region", "cell" or "any"; "" when id is not
     * standard. Documentation only: it never constrains where a label is used.
     * @throws Nothing.
     */
    const char* conventionalLayer(LabelId id) const;

    /**
     * @brief Returns an external ontology identifier (Cell Ontology, SNOMED CT or
     * NCI Thesaurus) for a label.
     * @param id The label.
     * @return "" for every label in version 1.
     * @throws Nothing.
     */
    const char* ontologyRef(LabelId id) const;
};

/**
 * @brief Returns the version of the shipped vocabulary.
 * @return 1.
 * @throws Nothing.
 */
SLIDEIO_PROCESSOR_DATA_EXPORTS int32_t standardVocabularyVersion() noexcept;

} // namespace slideio::processor
