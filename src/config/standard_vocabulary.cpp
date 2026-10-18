// Requirements: UR-RES-17
#include <slideio/processor/config/standard_vocabulary.hpp>
#include <slideio/processor/core/error.hpp>

#include <cstring>

namespace slideio::processor {

namespace {

constexpr int32_t kVersion = 1;

struct Entry {
    const char* name;
    const char* layer;
};

// Position i holds the label with id i + 1. Ids are a persistence contract:
// append only, never reorder.
constexpr Entry kLabels[] = {
    {"tumour", "region or cell"}, {"stroma", "region or cell"}, {"necrosis", "region"},
    {"immune", "cell"},           {"lymphocyte", "cell"},       {"positive", "any"},
    {"negative", "any"},          {"artefact", "region"},       {"ignore", "region"},
    {"other", "any"},
};
constexpr int32_t kCount = static_cast<int32_t>(sizeof(kLabels) / sizeof(kLabels[0]));

// The entry of a standard id, or null.
const Entry* entryOf(LabelId id) noexcept {
    const int32_t v = id.value();
    return (v >= 1 && v <= kCount) ? &kLabels[v - 1] : nullptr;
}

} // namespace

const StandardVocabulary& StandardVocabulary::instance() noexcept {
    static const StandardVocabulary vocabulary;
    return vocabulary;
}

int32_t StandardVocabulary::version() const noexcept { return kVersion; }

int32_t StandardVocabulary::labelCount() const noexcept { return kCount; }

LabelId StandardVocabulary::labelAt(int32_t index) const {
    if (index < 0 || index >= kCount) {
        throw InvalidArgumentError("StandardVocabulary::labelAt: index out of range");
    }
    return LabelId(index + 1);
}

const char* StandardVocabulary::labelName(LabelId id) const {
    const Entry* e = entryOf(id);
    return e ? e->name : "";
}

bool StandardVocabulary::findLabel(const char* name, LabelId& out) const noexcept {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    for (int32_t i = 0; i < kCount; ++i) {
        if (std::strcmp(kLabels[i].name, name) == 0) {
            out = LabelId(i + 1);
            return true;
        }
    }
    return false;
}

bool StandardVocabulary::isStandard(LabelId id) const noexcept { return entryOf(id) != nullptr; }

const char* StandardVocabulary::conventionalLayer(LabelId id) const {
    const Entry* e = entryOf(id);
    return e ? e->layer : "";
}

const char* StandardVocabulary::ontologyRef(LabelId) const { return ""; }

int32_t standardVocabularyVersion() noexcept { return kVersion; }

} // namespace slideio::processor
