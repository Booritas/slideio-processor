// Requirements: UR-IMG-12, UR-ALG-4, UR-ALG-6, UR-ALG-8, UR-EXT-5
#include <slideio/processor/pipeline/operation_catalog.hpp>

#include <slideio/processor/core/error.hpp>

#include <pipeline/detail/type_contract.hpp>

#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <fstream>
#include <map>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <string>

namespace slideio::processor {

namespace {

bool isNullOrEmpty(const char* text) {
    return text == nullptr || text[0] == '\0';
}

bool sameText(const char* a, const char* b) {
    return a != nullptr && b != nullptr && std::strcmp(a, b) == 0;
}


// The columns of the support matrix, in the order of DataType.
constexpr DataType kMatrixTypes[] = {DataType::Byte,    DataType::Int8,    DataType::UInt16,  DataType::Int16,
                                     DataType::UInt32,  DataType::Int32,   DataType::UInt64,  DataType::Int64,
                                     DataType::Float16, DataType::Float32, DataType::Float64};

} // namespace

OperationFactory::~OperationFactory() = default;

TileRequirement OperationFactory::tileRequirement() const {
    return TileRequirement::Halo;
}

int64_t OperationFactory::workingSetBytesPerLevelPixel() const {
    return 0;
}

bool OperationFactory::producesObjects() const {
    return false;
}

int32_t OperationFactory::outputPlaneCount(int32_t inputPlaneCount) const {
    return inputPlaneCount;
}

int64_t OperationFactory::workingSetBytesPerTile() const {
    return 0;
}

// std::map keeps the names in ascending bytewise order (char compares as
// unsigned char) and never moves a node, so a name or a factory pointer handed
// out stays valid: nothing is ever removed. The transparent comparator finds a
// const char* without building a std::string, so lookups do not allocate.
class OperationCatalog::Impl {
public:
    OperationFactoryPtr find(const char* name, const char* caller) const {
        if (name == nullptr) {
            throw InvalidArgumentError((std::string(caller) + ": the operation name is null").c_str());
        }
        std::shared_lock<std::shared_mutex> lock(mutex);
        const auto it = factories.find(name);
        if (it == factories.end()) {
            lock.unlock();
            throw InvalidArgumentError(
                (std::string(caller) + ": no operation named '" + name + "' is registered").c_str());
        }
        return it->second;
    }

    mutable std::shared_mutex mutex;
    std::map<std::string, OperationFactoryPtr, std::less<>> factories;
};

OperationCatalog::OperationCatalog() : impl_(std::make_unique<Impl>()) {}

OperationCatalog::~OperationCatalog() = default;

OperationCatalog& OperationCatalog::instance() noexcept {
    static OperationCatalog catalog;
    return catalog;
}

void OperationCatalog::registerFactory(OperationFactoryPtr factory) {
    if (factory == nullptr) {
        throw InvalidArgumentError("OperationCatalog::registerFactory: the factory is null");
    }
    const char* name = factory->name();
    if (isNullOrEmpty(name)) {
        throw InvalidArgumentError("OperationCatalog::registerFactory: the factory's name is null or empty");
    }
    const std::string key = name;
    if (isNullOrEmpty(factory->correctnessReference())) {
        throw InvalidArgumentError(("OperationCatalog::registerFactory: operation '" + key +
                                    "' names no correctness reference (UR-ALG-6)")
                                       .c_str());
    }
    if (factory->complexityNote() == nullptr) {
        throw InvalidArgumentError(
            ("OperationCatalog::registerFactory: operation '" + key + "' has a null complexity note").c_str());
    }
    std::unique_lock<std::shared_mutex> lock(impl_->mutex);
    if (!impl_->factories.emplace(key, std::move(factory)).second) {
        lock.unlock();
        throw InvalidArgumentError(
            ("OperationCatalog::registerFactory: an operation named '" + key + "' is already registered").c_str());
    }
}

bool OperationCatalog::isRegistered(const char* name) const noexcept {
    if (name == nullptr) {
        return false;
    }
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    return impl_->factories.find(name) != impl_->factories.end();
}

OperationPtr OperationCatalog::create(const char* name, const OperationParameters& parameters) const {
    const OperationFactoryPtr factory = impl_->find(name, "OperationCatalog::create");
    const std::string label = std::string("OperationCatalog::create: operation '") + name + "'";

    OperationPtr operation;
    try {
        operation = factory->create(parameters);
    } catch (const ProcessorError&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception& e) {
        ErrorContext context;
        context.operationName = name;
        context.cause = e.what();
        throw InternalError((label + " failed in its factory").c_str(), context);
    } catch (...) {
        ErrorContext context;
        context.operationName = name;
        context.cause = "an exception not derived from std::exception";
        throw InternalError((label + " failed in its factory").c_str(), context);
    }
    if (operation == nullptr) {
        throw InternalError((label + ": the factory returned no operation").c_str());
    }

    const auto mismatch = [&label](const char* field) {
        return InternalError((label + ": " + field + " differs from its factory's").c_str());
    };
    if (!sameText(operation->name(), factory->name())) {
        throw mismatch("name");
    }
    if (operation->arity() != factory->arity()) {
        throw mismatch("arity");
    }
    if (operation->requiresUniformTypes() != factory->requiresUniformTypes()) {
        throw mismatch("requiresUniformTypes");
    }
    if (!(operation->supportedTypes() == factory->supportedTypes())) {
        throw mismatch("supportedTypes");
    }
    if (operation->producesObjects() != factory->producesObjects()) {
        throw mismatch("producesObjects");
    }
    return operation;
}

int32_t OperationCatalog::count() const noexcept {
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    return static_cast<int32_t>(impl_->factories.size());
}

const char* OperationCatalog::nameAt(int32_t index) const {
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    if (index < 0 || static_cast<size_t>(index) >= impl_->factories.size()) {
        const size_t size = impl_->factories.size();
        lock.unlock();
        throw InvalidArgumentError(("OperationCatalog::nameAt: index " + std::to_string(index) +
                                    " is out of range for " + std::to_string(size) + " operations")
                                       .c_str());
    }
    return std::next(impl_->factories.begin(), index)->first.c_str();
}

ChannelArity OperationCatalog::arity(const char* name) const {
    return impl_->find(name, "OperationCatalog::arity")->arity();
}

DataTypeSet OperationCatalog::supportedTypes(const char* name) const {
    return impl_->find(name, "OperationCatalog::supportedTypes")->supportedTypes();
}

Halo OperationCatalog::halo(const char* name) const {
    return impl_->find(name, "OperationCatalog::halo")->halo();
}

const char* OperationCatalog::correctnessReference(const char* name) const {
    return impl_->find(name, "OperationCatalog::correctnessReference")->correctnessReference();
}

const char* OperationCatalog::complexityNote(const char* name) const {
    return impl_->find(name, "OperationCatalog::complexityNote")->complexityNote();
}

TileRequirement OperationCatalog::tileRequirement(const char* name) const {
    return impl_->find(name, "OperationCatalog::tileRequirement")->tileRequirement();
}

int64_t OperationCatalog::workingSetBytesPerLevelPixel(const char* name) const {
    return impl_->find(name, "OperationCatalog::workingSetBytesPerLevelPixel")->workingSetBytesPerLevelPixel();
}

bool OperationCatalog::producesObjects(const char* name) const {
    return impl_->find(name, "OperationCatalog::producesObjects")->producesObjects();
}

int32_t OperationCatalog::outputPlaneCount(const char* name, int32_t inputPlaneCount) const {
    return impl_->find(name, "OperationCatalog::outputPlaneCount")->outputPlaneCount(inputPlaneCount);
}

int64_t OperationCatalog::workingSetBytesPerTile(const char* name) const {
    return impl_->find(name, "OperationCatalog::workingSetBytesPerTile")->workingSetBytesPerTile();
}

int64_t OperationCatalog::writeSupportMatrix(const char* path) const {
    if (isNullOrEmpty(path)) {
        throw InvalidArgumentError("OperationCatalog::writeSupportMatrix: the path is null or empty");
    }

    std::string text = "operation";
    for (const DataType type : kMatrixTypes) {
        text += ',';
        text += dataTypeName(type);
    }
    text += '\n';
    {
        // One consistent snapshot: the shared lock is held while the rows are read.
        std::shared_lock<std::shared_mutex> lock(impl_->mutex);
        for (const auto& entry : impl_->factories) {
            const DataTypeSet declared = entry.second->supportedTypes();
            text += entry.first;
            for (const DataType type : kMatrixTypes) {
                text += detail::operationAccepts(declared, type) ? ",yes" : ",no";
            }
            text += '\n';
        }
    }

    const std::string where = path;
    std::ofstream out(std::filesystem::u8path(where), std::ios::binary | std::ios::trunc);
    if (!out) {
        throw IoError(("OperationCatalog::writeSupportMatrix: cannot open '" + where + "' for writing").c_str());
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    if (!out) {
        throw IoError(("OperationCatalog::writeSupportMatrix: cannot write '" + where + "'").c_str());
    }
    return static_cast<int64_t>(text.size());
}

} // namespace slideio::processor
