// Requirements: UR-IMG-12, UR-ALG-4, UR-ALG-6, UR-ALG-8, UR-EXT-5
#include <slideio/processor/core/error.hpp>
#include <slideio/processor/pipeline/backend.hpp>
#include <slideio/processor/pipeline/operation.hpp>
#include <slideio/processor/pipeline/operation_catalog.hpp>
#include <slideio/processor/pipeline/operation_parameters.hpp>

#include <pipeline/detail/type_contract.hpp>

#include <pipeline/public_only/third_party_operation.hpp>
#include <pipeline/test_support/operation_conformance.hpp>
#include <pipeline/test_support/test_operations.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using slideio::processor::ChannelArity;
using slideio::processor::DataType;
using slideio::processor::DataTypeSet;
using slideio::processor::Halo;
using slideio::processor::InternalError;
using slideio::processor::InvalidArgumentError;
using slideio::processor::IoError;
using slideio::processor::Operation;
using slideio::processor::OperationCatalog;
using slideio::processor::OperationContext;
using slideio::processor::OperationFactory;
using slideio::processor::OperationFactoryPtr;
using slideio::processor::OperationParameters;
using slideio::processor::OperationPtr;
using slideio::processor::TileRequirement;
namespace detail = slideio::processor::detail;
namespace ts = slideio::processor::testing_support;

namespace {

// What a StubFactory declares, and what the operation it creates declares when
// a test wants the two to disagree.
struct StubSpec {
    std::string name;
    ChannelArity arity = ChannelArity::PerChannel;
    DataTypeSet types = DataTypeSet().add(DataType::Byte);
    Halo halo = Halo::none();
    Halo operationHalo = Halo::none();
    DataTypeSet operationTypes = DataTypeSet().add(DataType::Byte);
    TileRequirement tileRequirement = TileRequirement::Halo;
    int64_t workingSet = 0;
    bool producesObjects = false;
    bool operationProducesObjects = false;
    const char* reference = "Analytic";
    const char* note = "O(n)";
    bool returnNull = false;
};

class StubOperation : public Operation {
public:
    explicit StubOperation(const StubSpec& spec) : spec_(spec) {}
    const char* name() const override { return spec_.name.c_str(); }
    ChannelArity arity() const override { return spec_.arity; }
    bool requiresUniformTypes() const override { return false; }
    DataTypeSet supportedTypes() const override { return spec_.operationTypes; }
    Halo halo() const override { return spec_.operationHalo; }
    DataType outputType(DataType type) const override { return type; }
    const OperationParameters& parameters() const override { return parameters_; }
    void apply(OperationContext&) override {}
    TileRequirement tileRequirement() const override { return spec_.tileRequirement; }
    bool producesObjects() const override { return spec_.operationProducesObjects; }

private:
    StubSpec spec_;
    OperationParameters parameters_;
};

class StubFactory : public OperationFactory {
public:
    explicit StubFactory(StubSpec spec) : spec_(std::move(spec)) {}
    OperationPtr create(const OperationParameters&) const override {
        if (spec_.returnNull) {
            return nullptr;
        }
        return std::make_shared<StubOperation>(spec_);
    }
    const char* name() const override { return spec_.name.c_str(); }
    ChannelArity arity() const override { return spec_.arity; }
    bool requiresUniformTypes() const override { return false; }
    DataTypeSet supportedTypes() const override { return spec_.types; }
    Halo halo() const override { return spec_.halo; }
    const char* correctnessReference() const override { return spec_.reference; }
    const char* complexityNote() const override { return spec_.note; }
    TileRequirement tileRequirement() const override { return spec_.tileRequirement; }
    int64_t workingSetBytesPerLevelPixel() const override { return spec_.workingSet; }
    bool producesObjects() const override { return spec_.producesObjects; }
    int32_t outputPlaneCount(int32_t n) const override { return n + 1; }

protected:
    StubSpec spec_;
};

// A factory that declares WholeLevel while the operation it creates says Halo.
class LyingFactory : public StubFactory {
public:
    using StubFactory::StubFactory;
    TileRequirement tileRequirement() const override { return TileRequirement::WholeLevel; }
};

// A factory whose create() throws something: a std::exception, or an int.
class ThrowingFactory : public StubFactory {
public:
    ThrowingFactory(StubSpec spec, bool standard) : StubFactory(std::move(spec)), standard_(standard) {}
    OperationPtr create(const OperationParameters&) const override {
        if (standard_) {
            throw std::runtime_error("factory exploded");
        }
        throw 42;
    }

private:
    bool standard_;
};

StubSpec specNamed(const std::string& name) {
    StubSpec spec;
    spec.name = name;
    return spec;
}

OperationFactoryPtr stub(const StubSpec& spec) {
    return std::make_shared<StubFactory>(spec);
}

// The catalog is a process singleton and tests may repeat, so register once.
void registerOnce(const OperationFactoryPtr& factory) {
    if (!OperationCatalog::instance().isRegistered(factory->name())) {
        OperationCatalog::instance().registerFactory(factory);
    }
}

template <typename Fn>
std::string messageOf(Fn&& fn) {
    try {
        fn();
    } catch (const slideio::processor::ProcessorError& e) {
        return e.what();
    }
    return "<no error>";
}

int32_t indexOf(const char* name) {
    const OperationCatalog& catalog = OperationCatalog::instance();
    for (int32_t i = 0; i < catalog.count(); ++i) {
        if (std::string(catalog.nameAt(i)) == name) {
            return i;
        }
    }
    return -1;
}

} // namespace

TEST(OperationCatalog, RegisterQueryCreate) {
    OperationCatalog& catalog = OperationCatalog::instance();
    const std::string name = "test.RegisterQueryCreate.threshold";
    StubSpec spec = specNamed(name);
    spec.arity = ChannelArity::Joint;
    spec.types = spec.operationTypes = DataTypeSet().add(DataType::Byte).add(DataType::Float32);
    spec.halo = spec.operationHalo = Halo::uniform(2);
    spec.tileRequirement = TileRequirement::WholeLevel;
    spec.workingSet = 24;
    spec.producesObjects = spec.operationProducesObjects = true;
    spec.reference = "Analytic: a unit test";
    spec.note = "O(n log n)";

    EXPECT_FALSE(catalog.isRegistered(name.c_str()));
    registerOnce(stub(spec));
    EXPECT_TRUE(catalog.isRegistered(name.c_str()));
    EXPECT_FALSE(catalog.isRegistered(nullptr));
    EXPECT_FALSE(catalog.isRegistered("test.RegisterQueryCreate.absent"));

    EXPECT_EQ(catalog.arity(name.c_str()), ChannelArity::Joint);
    EXPECT_TRUE(catalog.supportedTypes(name.c_str()) == spec.types);
    EXPECT_EQ(catalog.halo(name.c_str()).left, 2);
    EXPECT_EQ(catalog.halo(name.c_str()).bottom, 2);
    EXPECT_STREQ(catalog.correctnessReference(name.c_str()), "Analytic: a unit test");
    EXPECT_STREQ(catalog.complexityNote(name.c_str()), "O(n log n)");
    EXPECT_EQ(catalog.tileRequirement(name.c_str()), TileRequirement::WholeLevel);
    EXPECT_EQ(catalog.workingSetBytesPerLevelPixel(name.c_str()), 24);
    EXPECT_TRUE(catalog.producesObjects(name.c_str()));
    EXPECT_EQ(catalog.outputPlaneCount(name.c_str(), 3), 4);

    const OperationPtr op = catalog.create(name.c_str(), OperationParameters());
    ASSERT_NE(op, nullptr);
    EXPECT_EQ(op->name(), name);
    EXPECT_EQ(op->tileRequirement(), TileRequirement::WholeLevel);
}

TEST(OperationCatalog, DefaultFactoryDeclarations) {
    // A factory that overrides none of the whole-level, object and plane-count virtuals answers as Operation does.
    class Plain : public OperationFactory {
    public:
        OperationPtr create(const OperationParameters&) const override { return nullptr; }
        const char* name() const override { return "test.DefaultFactoryDeclarations.plain"; }
        ChannelArity arity() const override { return ChannelArity::PerChannel; }
        bool requiresUniformTypes() const override { return false; }
        DataTypeSet supportedTypes() const override { return DataTypeSet(); }
        Halo halo() const override { return Halo::none(); }
        const char* correctnessReference() const override { return "x"; }
        const char* complexityNote() const override { return ""; }
    } plain;
    EXPECT_EQ(plain.tileRequirement(), TileRequirement::Halo);
    EXPECT_EQ(plain.workingSetBytesPerLevelPixel(), 0);
    EXPECT_EQ(plain.workingSetBytesPerTile(), 0);
    EXPECT_FALSE(plain.producesObjects());
    EXPECT_EQ(plain.outputPlaneCount(5), 5);
}

TEST(OperationCatalog, RejectsBadFactories) {
    OperationCatalog& catalog = OperationCatalog::instance();
    EXPECT_THROW(catalog.registerFactory(nullptr), InvalidArgumentError);

    EXPECT_THROW(catalog.registerFactory(stub(specNamed(""))), InvalidArgumentError);

    StubSpec noReference = specNamed("test.RejectsBadFactories.noReference");
    noReference.reference = "";
    EXPECT_THROW(catalog.registerFactory(stub(noReference)), InvalidArgumentError);
    noReference.reference = nullptr;
    EXPECT_THROW(catalog.registerFactory(stub(noReference)), InvalidArgumentError);
    EXPECT_FALSE(catalog.isRegistered("test.RejectsBadFactories.noReference"));

    StubSpec noNote = specNamed("test.RejectsBadFactories.noNote");
    noNote.note = nullptr;
    EXPECT_THROW(catalog.registerFactory(stub(noNote)), InvalidArgumentError);
    EXPECT_FALSE(catalog.isRegistered("test.RejectsBadFactories.noNote"));

    // An empty complexity note is allowed; only a null one is not.
    StubSpec emptyNote = specNamed("test.RejectsBadFactories.emptyNote");
    emptyNote.note = "";
    registerOnce(stub(emptyNote));
    EXPECT_TRUE(catalog.isRegistered("test.RejectsBadFactories.emptyNote"));

    const std::string duplicate = "test.RejectsBadFactories.duplicate";
    registerOnce(stub(specNamed(duplicate)));
    const std::string message = messageOf([&] { catalog.registerFactory(stub(specNamed(duplicate))); });
    EXPECT_NE(message.find(duplicate), std::string::npos) << message;
    EXPECT_THROW(catalog.registerFactory(stub(specNamed(duplicate))), InvalidArgumentError);
}

TEST(OperationCatalog, UnknownNameIsNamed) {
    const OperationCatalog& catalog = OperationCatalog::instance();
    const OperationParameters parameters;
    EXPECT_THROW(catalog.create("test.nope", parameters), InvalidArgumentError);
    EXPECT_NE(messageOf([&] { catalog.create("test.nope", parameters); }).find("test.nope"), std::string::npos);
    EXPECT_NE(messageOf([&] { catalog.arity("test.nope"); }).find("test.nope"), std::string::npos);
    EXPECT_THROW(catalog.supportedTypes("test.nope"), InvalidArgumentError);
    EXPECT_THROW(catalog.halo("test.nope"), InvalidArgumentError);
    EXPECT_THROW(catalog.correctnessReference("test.nope"), InvalidArgumentError);
    EXPECT_THROW(catalog.complexityNote("test.nope"), InvalidArgumentError);
    EXPECT_THROW(catalog.tileRequirement("test.nope"), InvalidArgumentError);
    EXPECT_THROW(catalog.workingSetBytesPerLevelPixel("test.nope"), InvalidArgumentError);
    EXPECT_THROW(catalog.producesObjects("test.nope"), InvalidArgumentError);
    EXPECT_THROW(catalog.outputPlaneCount("test.nope", 1), InvalidArgumentError);
    EXPECT_THROW(catalog.create(nullptr, parameters), InvalidArgumentError);
    EXPECT_THROW(catalog.arity(nullptr), InvalidArgumentError);
}

TEST(OperationCatalog, NamesAreSorted) {
    OperationCatalog& catalog = OperationCatalog::instance();
    registerOnce(stub(specNamed("test.NamesAreSorted.b")));
    registerOnce(stub(specNamed("test.NamesAreSorted.a")));
    const int32_t a = indexOf("test.NamesAreSorted.a");
    const int32_t b = indexOf("test.NamesAreSorted.b");
    ASSERT_GE(a, 0);
    ASSERT_GE(b, 0);
    EXPECT_LT(a, b);
    for (int32_t i = 1; i < catalog.count(); ++i) {
        EXPECT_LT(std::string(catalog.nameAt(i - 1)), std::string(catalog.nameAt(i)));
    }
    EXPECT_THROW(catalog.nameAt(-1), InvalidArgumentError);
    EXPECT_THROW(catalog.nameAt(catalog.count()), InvalidArgumentError);
}

TEST(OperationCatalog, CreatedOperationMustMatchItsFactory) {
    OperationCatalog& catalog = OperationCatalog::instance();
    const OperationParameters parameters;

    StubSpec types = specNamed("test.CreatedOperationMustMatchItsFactory.types");
    types.operationTypes = DataTypeSet().add(DataType::UInt16);
    registerOnce(stub(types));
    EXPECT_THROW(catalog.create(types.name.c_str(), parameters), InternalError);
    EXPECT_NE(messageOf([&] { catalog.create(types.name.c_str(), parameters); }).find("supportedTypes"),
              std::string::npos);

    StubSpec objects = specNamed("test.CreatedOperationMustMatchItsFactory.objects");
    objects.operationProducesObjects = true;
    registerOnce(stub(objects));
    EXPECT_NE(messageOf([&] { catalog.create(objects.name.c_str(), parameters); }).find("producesObjects"),
              std::string::npos);

    // Parameter-dependent declarations are not compared: the factory describes the defaults.
    StubSpec tile = specNamed("test.CreatedOperationMustMatchItsFactory.tile");
    tile.operationHalo = Halo::uniform(3);
    registerOnce(std::make_shared<LyingFactory>(tile));
    const OperationPtr tolerated = catalog.create(tile.name.c_str(), parameters);
    ASSERT_NE(tolerated, nullptr);
    EXPECT_EQ(tolerated->halo().left, 3);
    EXPECT_EQ(catalog.halo(tile.name.c_str()).left, 0);

    StubSpec nothing = specNamed("test.CreatedOperationMustMatchItsFactory.null");
    nothing.returnNull = true;
    registerOnce(stub(nothing));
    EXPECT_THROW(catalog.create(nothing.name.c_str(), parameters), InternalError);
}

TEST(OperationCatalog, FactoryExceptionsBecomeInternalErrors) {
    OperationCatalog& catalog = OperationCatalog::instance();
    const OperationParameters parameters;
    const std::string standard = "test.FactoryExceptionsBecomeInternalErrors.standard";
    const std::string other = "test.FactoryExceptionsBecomeInternalErrors.other";
    registerOnce(std::make_shared<ThrowingFactory>(specNamed(standard), true));
    registerOnce(std::make_shared<ThrowingFactory>(specNamed(other), false));
    for (const std::string& name : {standard, other}) {
        try {
            catalog.create(name.c_str(), parameters);
            ADD_FAILURE() << name << ": no exception";
        } catch (const InternalError& e) {
            EXPECT_NE(std::string(e.what()).find("failed in its factory"), std::string::npos);
            EXPECT_STREQ(e.operationName(), name.c_str());
            ASSERT_NE(e.cause(), nullptr);
            EXPECT_EQ(std::string(e.cause()),
                      name == standard ? "factory exploded" : "an exception not derived from std::exception");
        }
    }
}

TEST(OperationCatalog, SupportMatrixMatchesTheRuntimeContract) {
    ts::registerTestOperations();
    registerOnce(thirdparty::makeInvertFactory());
    StubSpec everything = specNamed("test.SupportMatrix.everything");
    everything.types = everything.operationTypes = DataTypeSet::all();
    registerOnce(stub(everything));
    OperationCatalog& catalog = OperationCatalog::instance();

    const auto path = std::filesystem::temp_directory_path() / "slideio_processor_support_matrix_test.csv";
    const int64_t written = catalog.writeSupportMatrix(path.u8string().c_str());

    std::ifstream in(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::filesystem::remove(path);
    EXPECT_EQ(written, static_cast<int64_t>(text.size()));
    EXPECT_EQ(text.find('\r'), std::string::npos);
    ASSERT_FALSE(text.empty());
    EXPECT_EQ(text.back(), '\n');

    std::vector<std::string> lines;
    std::istringstream stream(text);
    for (std::string line; std::getline(stream, line);) {
        lines.push_back(line);
    }
    ASSERT_EQ(static_cast<int32_t>(lines.size()), catalog.count() + 1);
    EXPECT_EQ(lines[0], "operation,Byte,Int8,UInt16,Int16,UInt32,Int32,UInt64,Int64,Float16,Float32,Float64");

    const DataType types[] = {DataType::Byte,   DataType::Int8,    DataType::UInt16,  DataType::Int16,
                              DataType::UInt32, DataType::Int32,   DataType::UInt64,  DataType::Int64,
                              DataType::Float16, DataType::Float32, DataType::Float64};
    const auto& backend = slideio::processor::Backend::instance();
    for (int32_t i = 0; i < catalog.count(); ++i) {
        const std::string name = catalog.nameAt(i);
        const std::string& line = lines[static_cast<size_t>(i) + 1];
        std::vector<std::string> cells;
        std::istringstream row(line);
        for (std::string cell; std::getline(row, cell, ',');) {
            cells.push_back(cell);
        }
        ASSERT_EQ(cells.size(), 12u) << line;
        EXPECT_EQ(cells[0], name);
        for (size_t t = 0; t < 11; ++t) {
            const bool accepted = detail::operationAccepts(catalog.supportedTypes(name.c_str()), types[t]);
            EXPECT_EQ(cells[t + 1], accepted ? "yes" : "no") << name << " column " << t + 1;
            if (types[t] == DataType::UInt32 && !backend.supports(DataType::UInt32)) {
                EXPECT_EQ(cells[t + 1], "no") << name;
            }
        }
        if (name == "test.SupportMatrix.everything") {
            // Declares every type, so its row is exactly what the backend computes.
            for (size_t t = 0; t < 11; ++t) {
                EXPECT_EQ(cells[t + 1], backend.supports(types[t]) ? "yes" : "no") << "column " << t + 1;
            }
        }
    }
}

TEST(OperationCatalog, OperationAcceptsNeedsBothTheDeclarationAndTheBackend) {
    const DataTypeSet declared = DataTypeSet().add(DataType::Byte).add(DataType::UInt32);
    EXPECT_TRUE(detail::operationAccepts(declared, DataType::Byte));
    EXPECT_FALSE(detail::operationAccepts(declared, DataType::UInt16));
    EXPECT_EQ(detail::operationAccepts(declared, DataType::UInt32),
              slideio::processor::Backend::instance().supports(DataType::UInt32));
    EXPECT_FALSE(detail::operationAccepts(DataTypeSet::all(), DataType::Unknown));
}

TEST(OperationCatalog, SupportMatrixReportsAnUnwritablePath) {
    const OperationCatalog& catalog = OperationCatalog::instance();
    const auto path = std::filesystem::temp_directory_path() / "slideio_processor_no_such_dir" / "matrix.csv";
    const std::string text = path.u8string();
    EXPECT_THROW(catalog.writeSupportMatrix(text.c_str()), IoError);
    const std::string message = messageOf([&] { catalog.writeSupportMatrix(text.c_str()); });
    EXPECT_NE(message.find("cannot open the file for writing"), std::string::npos) << message;
    EXPECT_EQ(message.find("matrix.csv"), std::string::npos) << message;
    EXPECT_EQ(message.find("slideio_processor_no_such_dir"), std::string::npos) << message;
    EXPECT_THROW(catalog.writeSupportMatrix(nullptr), InvalidArgumentError);
    EXPECT_THROW(catalog.writeSupportMatrix(""), InvalidArgumentError);
}

TEST(OperationCatalog, ConformanceHelperPassesForTestOperations) {
    ts::registerTestOperations();
    ts::expectConformant("test.threshold");
    ts::expectConformant("test.boxMean");
    ts::expectConformant("test.components");
    EXPECT_TRUE(OperationCatalog::instance().producesObjects("test.components"));
    EXPECT_FALSE(OperationCatalog::instance().producesObjects("test.threshold"));
}

TEST(OperationCatalog, ThirdPartyOperationRoundTripsThroughTheCatalog) {
    registerOnce(thirdparty::makeInvertFactory());
    ts::expectConformant("thirdparty.invert");
    const OperationCatalog& catalog = OperationCatalog::instance();
    const OperationPtr op = catalog.create("thirdparty.invert", OperationParameters().setInt("bias", 7));
    int64_t bias = 0;
    EXPECT_TRUE(op->parameters().getInt("bias", bias));
    EXPECT_EQ(bias, 7);
    EXPECT_EQ(catalog.halo("thirdparty.invert").top, 1);
}

TEST(OperationCatalog, ThreadSafeQueries) {
    ts::registerTestOperations();
    OperationCatalog& catalog = OperationCatalog::instance();
    std::atomic<int32_t> failures{0};
    std::atomic<bool> go{false};

    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            while (!go.load()) {
            }
            for (int i = 0; i < 200; ++i) {
                try {
                    const OperationPtr op = catalog.create("test.threshold", OperationParameters());
                    if (op == nullptr || !catalog.isRegistered("test.components") || catalog.count() <= 0 ||
                        catalog.arity("test.boxMean") != ChannelArity::PerChannel ||
                        catalog.nameAt(0) == nullptr) {
                        ++failures;
                    }
                } catch (...) {
                    ++failures;
                }
            }
        });
    }
    threads.emplace_back([&] {
        while (!go.load()) {
        }
        for (int i = 0; i < 100; ++i) {
            try {
                registerOnce(stub(specNamed("test.ThreadSafeQueries.n" + std::to_string(i))));
            } catch (...) {
                ++failures;
            }
        }
    });
    go = true;
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(failures.load(), 0);
}

TEST(OperationCatalog, ParameterDependentDeclarationsComeFromTheInstance) {
    ts::registerTestOperations();
    const OperationCatalog& catalog = OperationCatalog::instance();
    const OperationPtr op = catalog.create("test.boxMean", OperationParameters().setInt("radius", 5));
    ASSERT_NE(op, nullptr);
    EXPECT_EQ(op->halo().left, 5);
    EXPECT_EQ(op->halo().bottom, 5);
    // The catalog answers for the default parameters (radius 1).
    EXPECT_EQ(catalog.halo("test.boxMean").left, 1);
}

namespace {

// A factory whose operations may vary between runs.
class NondeterministicFactory : public StubFactory {
public:
    using StubFactory::StubFactory;
    bool isDeterministic() const override { return false; }
};

} // namespace

TEST(OperationCatalog, DeterministicByDefault) {
    // A factory that does not override isDeterministic() is deterministic; the built-ins do not override it.
    OperationFactoryPtr plain = stub(specNamed("test.DeterministicByDefault.plain"));
    EXPECT_TRUE(plain->isDeterministic());
    registerOnce(plain);
    const OperationCatalog& catalog = OperationCatalog::instance();
    EXPECT_TRUE(catalog.isDeterministic("test.DeterministicByDefault.plain"));
    if (catalog.isRegistered("segment.contours")) {
        EXPECT_TRUE(catalog.isDeterministic("segment.contours"));
    }
    EXPECT_THROW(catalog.isDeterministic("test.DeterministicByDefault.unknown"), InvalidArgumentError);
    EXPECT_THROW(catalog.isDeterministic(nullptr), InvalidArgumentError);
}

TEST(OperationCatalog, ReportsANondeterministicFactory) {
    registerOnce(std::make_shared<NondeterministicFactory>(specNamed("test.ReportsANondeterministicFactory.gpu")));
    EXPECT_FALSE(OperationCatalog::instance().isDeterministic("test.ReportsANondeterministicFactory.gpu"));
}
