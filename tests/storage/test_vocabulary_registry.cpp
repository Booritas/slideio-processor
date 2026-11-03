// Requirements: UR-RES-17, UR-RES-18, UR-RES-19
#include "storage/helper_process.hpp"
#include "storage/store_fixture.hpp"

#include <slideio/processor/config/standard_vocabulary.hpp>
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/storage/event_journal.hpp>
#include <slideio/processor/storage/store.hpp>
#include <slideio/processor/storage/vocabulary_registry.hpp>

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <string>

using namespace slideio::processor;

namespace {

class VocabularyRegistryTest : public StoreFixture {};

// Runs one statement on a raw connection to the store file, in this process.
// Rule (helper_process.hpp): the test's own SQLite copy, so use it only while no Store has
// the file open; a write beside a live store goes through execInAnotherProcess.
void rawExec(const std::string& path, const char* sql) {
    sqlite3* db = nullptr;
    ASSERT_EQ(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr), SQLITE_OK);
    sqlite3_busy_timeout(db, 5000);
    char* error = nullptr;
    EXPECT_EQ(sqlite3_exec(db, sql, nullptr, nullptr, &error), SQLITE_OK) << (error != nullptr ? error : "");
    sqlite3_free(error);
    sqlite3_close(db);
}

}  // namespace

TEST_F(VocabularyRegistryTest, SeededWithTheStandardTen) {
    VocabularyRegistry& V = store().vocabulary();
    EXPECT_EQ(V.recordedStandardVersion(), 1);
    EXPECT_EQ(V.count(), 10);
    LabelId t;
    EXPECT_TRUE(V.find("tumour", t));
    EXPECT_EQ(t, LabelId(1));
    EXPECT_EQ(V.origin(t), LabelOrigin::Standard);
    EXPECT_TRUE(V.isRecognised(t));

    const StandardVocabulary& standard = StandardVocabulary::instance();
    for (int32_t i = 0; i < 10; ++i) {
        const LabelId id = V.at(i);
        EXPECT_EQ(id, LabelId(i + 1));
        EXPECT_STREQ(V.name(id), standard.labelName(id));
        EXPECT_EQ(V.origin(id), LabelOrigin::Standard);
        EXPECT_TRUE(V.isRecognised(id));
    }
    EXPECT_THROW(V.at(10), InvalidArgumentError);
    EXPECT_THROW(V.at(-1), InvalidArgumentError);
}

TEST_F(VocabularyRegistryTest, FindFoldsCaseAndAliases) {
    VocabularyRegistry& V = store().vocabulary();
    LabelId id;
    EXPECT_TRUE(V.find("Tumor", id));
    EXPECT_EQ(id, LabelId(1));
    EXPECT_STREQ(V.name(id), "tumour");
    EXPECT_TRUE(V.find("TUMOUR", id));
    EXPECT_EQ(id, LabelId(1));
    EXPECT_TRUE(V.find("ARTIFACT", id));
    EXPECT_EQ(id, LabelId(8));
    EXPECT_STREQ(V.name(id), "artefact");
    EXPECT_TRUE(V.find("Immune cells", id));
    EXPECT_EQ(id, LabelId(4));
    EXPECT_TRUE(V.find("Ignore*", id));
    EXPECT_EQ(id, LabelId(9));
    EXPECT_STREQ(V.name(id), "ignore");

    id = LabelId(77);
    EXPECT_FALSE(V.find("tissue", id));
    EXPECT_FALSE(V.find("", id));
    EXPECT_FALSE(V.find(nullptr, id));
    EXPECT_EQ(id, LabelId(77));  // unchanged when not found
}

TEST_F(VocabularyRegistryTest, AsteriskIsLiteralNotAWildcard) {
    VocabularyRegistry& V = store().vocabulary();
    LabelId id;
    EXPECT_FALSE(V.find("ignore**", id));
    EXPECT_FALSE(V.find("ign*", id));
    EXPECT_FALSE(V.find("*", id));
    EXPECT_FALSE(V.find("tumor*", id));
    EXPECT_FALSE(V.find("immune cells*", id));
    EXPECT_FALSE(V.find("immune  cells", id));
}

TEST_F(VocabularyRegistryTest, LocalLabelsLiveAbove65535AndAreJournalled) {
    LabelId k;
    {
        VocabularyRegistry& V = store().vocabulary();
        EventCursorPtr cursor = store().events().subscribe(EventJournal::kFromBeginning);
        EXPECT_FALSE(cursor->next(0));  // seeding the standard ten is not an event

        k = V.addLocal("ki67_positive");
        EXPECT_EQ(k, LabelId(65536));
        EXPECT_EQ(V.origin(k), LabelOrigin::WorkspaceLocal);
        EXPECT_TRUE(V.isRecognised(k));
        EXPECT_STREQ(V.name(k), "ki67_positive");
        EXPECT_EQ(V.addLocal("KI67_POSITIVE"), k);  // found, not re-added
        EXPECT_EQ(V.addLocal("Tumor"), LabelId(1));
        EXPECT_EQ(V.count(), 11);
        EXPECT_EQ(V.at(10), k);

        const LabelId second = V.addLocal("er_positive");
        EXPECT_EQ(second, LabelId(65537));
        EXPECT_EQ(V.at(11), second);

        EXPECT_THROW(V.addLocal(""), InvalidArgumentError);
        EXPECT_THROW(V.addLocal(nullptr), InvalidArgumentError);

        // Exactly one LabelAdded event per added label, carrying its id.
        const LabelId added[] = {k, second};
        for (LabelId label : added) {
            ASSERT_TRUE(cursor->next(0));
            EXPECT_EQ(cursor->current().kind(), EventKind::LabelAdded);
            EXPECT_EQ(cursor->current().label(), label);
            EXPECT_EQ(cursor->current().objectCount(), 0);
            EXPECT_TRUE(cursor->current().boundingBox() == (Rect64{0, 0, 0, 0}));
            EXPECT_FALSE(cursor->current().layer().isValid());
            EXPECT_FALSE(cursor->current().stage().isValid());
        }
        EXPECT_FALSE(cursor->next(0));
    }

    StorePtr reopened = reopen(OpenMode::ReadOnly);
    const VocabularyRegistry& V = reopened->vocabulary();
    EXPECT_EQ(V.count(), 12);
    LabelId found;
    EXPECT_TRUE(V.find("Ki67_Positive", found));
    EXPECT_EQ(found, k);
    EXPECT_EQ(V.origin(k), LabelOrigin::WorkspaceLocal);
    EXPECT_STREQ(V.name(k), "ki67_positive");
    EXPECT_EQ(V.recordedStandardVersion(), 1);
}

TEST_F(VocabularyRegistryTest, NameIsCanonicalAndPointerIsStable) {
    VocabularyRegistry& V = store().vocabulary();
    const LabelId first = V.addLocal("Mixed_Case");
    const char* name = V.name(first);
    EXPECT_STREQ(name, "Mixed_Case");
    for (int i = 0; i < 50; ++i) V.addLocal(("label_" + std::to_string(i)).c_str());
    EXPECT_EQ(V.name(first), name);
    EXPECT_STREQ(name, "Mixed_Case");
    EXPECT_EQ(V.addLocal("mixed_case"), first);
    EXPECT_STREQ(V.name(first), "Mixed_Case");
}

TEST_F(VocabularyRegistryTest, UnknownIdsThrow) {
    VocabularyRegistry& V = store().vocabulary();
    EXPECT_THROW(V.name(LabelId(11)), InvalidArgumentError);
    EXPECT_THROW(V.origin(LabelId(65536)), InvalidArgumentError);
    EXPECT_THROW(V.name(LabelId()), InvalidArgumentError);
    EXPECT_FALSE(V.isRecognised(LabelId(11)));
    EXPECT_FALSE(V.isRecognised(LabelId()));
}

TEST_F(VocabularyRegistryTest, NewerStandardLabelIsUnrecognisedButIntact) {  // UR-RES-17
    store().close();
    rawExec(pathUtf8(), "INSERT INTO labels(id, name, origin) VALUES (11, 'mitotic', 0)");
    StorePtr reopened = reopen(OpenMode::ReadWrite);
    VocabularyRegistry& V = reopened->vocabulary();
    EXPECT_EQ(V.count(), 11);
    LabelId id;
    EXPECT_TRUE(V.find("MITOTIC", id));
    EXPECT_EQ(id, LabelId(11));
    EXPECT_STREQ(V.name(id), "mitotic");
    EXPECT_EQ(V.origin(id), LabelOrigin::Standard);
    EXPECT_FALSE(V.isRecognised(id));
    EXPECT_TRUE(V.isRecognised(LabelId(10)));
    EXPECT_EQ(V.at(10), LabelId(11));
    EXPECT_EQ(V.addLocal("mitotic"), LabelId(11));  // already held
    EXPECT_EQ(V.addLocal("local"), LabelId(65536));  // local ids never enter the standard range
    EXPECT_EQ(V.recordedStandardVersion(), 1);
}

TEST_F(VocabularyRegistryTest, SeesRowsAddedThroughAnotherConnection) {
    VocabularyRegistry& V = store().vocabulary();
    EXPECT_EQ(V.count(), 10);
    // Another process adds a row while this store stays open.
    ASSERT_EQ(execInAnotherProcess(pathUtf8(), "INSERT INTO labels(id, name, origin) VALUES (65536, 'raw', 1)"), 0);
    EXPECT_EQ(V.count(), 11);
    LabelId id;
    EXPECT_TRUE(V.find("raw", id));
    EXPECT_EQ(id, LabelId(65536));
    EXPECT_EQ(V.addLocal("next"), LabelId(65537));
}

TEST_F(VocabularyRegistryTest, ImportPolicyPersists) {
    {
        VocabularyRegistry& V = store().vocabulary();
        EXPECT_EQ(V.importPolicy(), ImportPolicy::Strict);
        V.setImportPolicy(ImportPolicy::Permissive);
        EXPECT_EQ(V.importPolicy(), ImportPolicy::Permissive);
        EXPECT_THROW(V.setImportPolicy(static_cast<ImportPolicy>(99)), InvalidArgumentError);
        EXPECT_EQ(V.importPolicy(), ImportPolicy::Permissive);
    }
    StorePtr ro = reopen(OpenMode::ReadOnly);
    EXPECT_EQ(ro->vocabulary().importPolicy(), ImportPolicy::Permissive);
    EXPECT_THROW(ro->vocabulary().setImportPolicy(ImportPolicy::Strict), IoError);
    EXPECT_EQ(ro->vocabulary().importPolicy(), ImportPolicy::Permissive);
}

TEST_F(VocabularyRegistryTest, ImportPolicyIsReadFresh) {
    VocabularyRegistry& V = store().vocabulary();
    EXPECT_EQ(V.importPolicy(), ImportPolicy::Strict);
    // Another process changes the policy while this store stays open.
    ASSERT_EQ(execInAnotherProcess(pathUtf8(), "UPDATE meta SET value = 1 WHERE key = 'import_policy'"), 0);
    EXPECT_EQ(V.importPolicy(), ImportPolicy::Permissive);
}

TEST_F(VocabularyRegistryTest, ReadOnlyAnswersQueriesAndRefusesAdditions) {
    StorePtr ro = reopen(OpenMode::ReadOnly);
    VocabularyRegistry& V = ro->vocabulary();
    EXPECT_EQ(V.count(), 10);
    LabelId id;
    EXPECT_TRUE(V.find("tumor", id));
    EXPECT_THROW(V.addLocal("new"), IoError);
    try {
        V.addLocal("new");
    } catch (const IoError& e) {
        EXPECT_NE(std::string(e.what()).find("read-only"), std::string::npos);
    }
    EXPECT_EQ(V.count(), 10);
}

TEST_F(VocabularyRegistryTest, ClosedStoreThrowsOrAnswersDefaults) {
    VocabularyRegistry& V = store().vocabulary();
    store().close();
    EXPECT_THROW(V.addLocal("x"), IoError);
    EXPECT_THROW(V.at(0), IoError);
    EXPECT_THROW(V.name(LabelId(1)), IoError);
    EXPECT_THROW(V.setImportPolicy(ImportPolicy::Strict), IoError);
    LabelId id;
    EXPECT_FALSE(V.find("tumour", id));
    EXPECT_EQ(V.count(), 0);
    EXPECT_EQ(V.recordedStandardVersion(), 0);
    EXPECT_EQ(V.importPolicy(), ImportPolicy::Strict);
}
