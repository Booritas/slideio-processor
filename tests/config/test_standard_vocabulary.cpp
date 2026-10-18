// Requirements: UR-RES-17
#include <slideio/processor/config/standard_vocabulary.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/core/ids.hpp>

#include <gtest/gtest.h>

#include <cstdint>

using slideio::processor::InvalidArgumentError;
using slideio::processor::LabelId;
using slideio::processor::StandardVocabulary;
using slideio::processor::standardVocabularyVersion;

// G7: the ids are a persistence contract.
TEST(StandardVocabulary, TenLabelsWithStableIdsInTableOrder) {
    const StandardVocabulary& v = StandardVocabulary::instance();
    EXPECT_EQ(v.version(), 1);
    EXPECT_EQ(standardVocabularyVersion(), 1);
    const char* names[] = {"tumour",   "stroma",   "necrosis", "immune", "lymphocyte",
                           "positive", "negative", "artefact", "ignore", "other"};
    const char* layers[] = {"region or cell", "region or cell", "region", "cell", "cell",
                            "any",            "any",            "region", "region", "any"};
    ASSERT_EQ(v.labelCount(), 10);
    for (int32_t i = 0; i < 10; ++i) {
        EXPECT_EQ(v.labelAt(i), LabelId(i + 1));
        EXPECT_STREQ(v.labelName(LabelId(i + 1)), names[i]);
        EXPECT_STREQ(v.conventionalLayer(LabelId(i + 1)), layers[i]);
        EXPECT_STREQ(v.ontologyRef(LabelId(i + 1)), "");
        LabelId found;
        EXPECT_TRUE(v.findLabel(names[i], found));
        EXPECT_EQ(found, LabelId(i + 1));
        EXPECT_TRUE(v.isStandard(LabelId(i + 1)));
    }
    EXPECT_THROW(v.labelAt(-1), InvalidArgumentError);
    EXPECT_THROW(v.labelAt(10), InvalidArgumentError);
}

// Review Focus 5, G8, G9.
TEST(StandardVocabulary, LookupIsExactAndNonStandardIdsReadEmpty) {
    const StandardVocabulary& v = StandardVocabulary::instance();
    LabelId out(42);
    EXPECT_FALSE(v.findLabel("Tumour", out));
    EXPECT_FALSE(v.findLabel("tumour ", out));
    EXPECT_FALSE(v.findLabel("tissue", out)); // deliberately absent (SDS 02)
    EXPECT_FALSE(v.findLabel("", out));
    EXPECT_FALSE(v.findLabel(nullptr, out));
    for (LabelId id : {LabelId(), LabelId(0), LabelId(11), LabelId(-3)}) {
        EXPECT_FALSE(v.isStandard(id));
        EXPECT_STREQ(v.labelName(id), "");
        EXPECT_STREQ(v.conventionalLayer(id), "");
        EXPECT_STREQ(v.ontologyRef(id), "");
    }
}
