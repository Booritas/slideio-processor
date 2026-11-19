// Requirements: UR-EXT-1, UR-EXT-5, UR-ALG-4, UR-STG-4
#include <pipeline/test_support/operation_conformance.hpp>

#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>

#include <pipeline/detail/parameters_json.hpp>

#include <gtest/gtest.h>

#include <cstring>
#include <string>

namespace slideio::processor::testing_support {

namespace {

bool sameHalo(const Halo& a, const Halo& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

void expectSameDeclarations(const Operation& a, const Operation& b) {
    EXPECT_STREQ(a.name(), b.name());
    EXPECT_EQ(a.arity(), b.arity());
    EXPECT_EQ(a.requiresUniformTypes(), b.requiresUniformTypes());
    EXPECT_TRUE(a.supportedTypes() == b.supportedTypes());
    EXPECT_TRUE(sameHalo(a.halo(), b.halo()));
    EXPECT_EQ(a.tileRequirement(), b.tileRequirement());
    EXPECT_EQ(a.workingSetBytesPerLevelPixel(), b.workingSetBytesPerLevelPixel());
    EXPECT_EQ(a.producesObjects(), b.producesObjects());
    EXPECT_EQ(a.workingSetBytesPerTile(), b.workingSetBytesPerTile());
    // An operation refuses a plane count it does not take by throwing; -1 here.
    const auto planesOf = [](const Operation& op, int32_t n) {
        try {
            return op.outputPlaneCount(n);
        } catch (const ProcessorError&) {
            return -1;
        }
    };
    for (int32_t n = 1; n <= 4; ++n) {
        EXPECT_EQ(planesOf(a, n), planesOf(b, n)) << "outputPlaneCount(" << n << ")";
    }
}

} // namespace

void expectConformant(const char* name) {
    SCOPED_TRACE(std::string("operation ") + (name != nullptr ? name : "(null)"));
    const OperationCatalog& catalog = OperationCatalog::instance();
    ASSERT_TRUE(catalog.isRegistered(name));

    const OperationPtr op = catalog.create(name, OperationParameters());
    ASSERT_NE(op, nullptr);
    EXPECT_STREQ(op->name(), name);
    EXPECT_EQ(op->arity(), catalog.arity(name));
    EXPECT_TRUE(op->supportedTypes() == catalog.supportedTypes(name));
    EXPECT_EQ(op->producesObjects(), catalog.producesObjects(name));

    const OperationPtr again = catalog.create(op->name(), op->parameters());
    ASSERT_NE(again, nullptr);
    expectSameDeclarations(*op, *again);
    EXPECT_TRUE(detail::parametersEqual(op->parameters(), again->parameters()));
}

} // namespace slideio::processor::testing_support
