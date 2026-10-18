#include <slideio/processor/core/error.hpp>

#include <cstdint>
#include <string>
#include <utility>

namespace slideio::processor {

namespace {
std::string text(const char* s) { return s ? std::string(s) : std::string(); }
} // namespace

struct ProcessorError::Impl {
    ErrorCode code;
    std::string stageName;
    std::string operationName;
    std::string cause;
    bool hasTile;
    int64_t tileX;
    int64_t tileY;
    std::string what;
};

ProcessorError::ProcessorError(ErrorCode code, const char* message, const ErrorContext& context) {
    auto impl = std::make_shared<Impl>();
    impl->code = code;
    impl->stageName = text(context.stageName);
    impl->operationName = text(context.operationName);
    impl->cause = text(context.cause);
    impl->hasTile = context.hasTile;
    impl->tileX = context.hasTile ? context.tileX : 0;
    impl->tileY = context.hasTile ? context.tileY : 0;
    std::string w = text(message);
    if (!impl->stageName.empty()) {
        w += "; stage: " + impl->stageName;
    }
    if (!impl->operationName.empty()) {
        w += "; operation: " + impl->operationName;
    }
    if (impl->hasTile) {
        w += "; tile: (" + std::to_string(impl->tileX) + ", " + std::to_string(impl->tileY) + ")";
    }
    if (!impl->cause.empty()) {
        w += "; cause: " + impl->cause;
    }
    impl->what = std::move(w);
    impl_ = std::move(impl);
}

ProcessorError::ProcessorError(const ProcessorError& other) noexcept = default;
ProcessorError& ProcessorError::operator=(const ProcessorError& other) noexcept = default;
ProcessorError::~ProcessorError() = default;

const char* ProcessorError::what() const noexcept { return impl_->what.c_str(); }
ErrorCode ProcessorError::code() const noexcept { return impl_->code; }
const char* ProcessorError::stageName() const noexcept { return impl_->stageName.c_str(); }
const char* ProcessorError::operationName() const noexcept { return impl_->operationName.c_str(); }
const char* ProcessorError::cause() const noexcept { return impl_->cause.c_str(); }
bool ProcessorError::hasTile() const noexcept { return impl_->hasTile; }
int64_t ProcessorError::tileX() const noexcept { return impl_->tileX; }
int64_t ProcessorError::tileY() const noexcept { return impl_->tileY; }

InvalidArgumentError::InvalidArgumentError(const char* message, const ErrorContext& context)
    : ProcessorError(ErrorCode::InvalidArgument, message, context) {}

UnsupportedDataTypeError::UnsupportedDataTypeError(const char* message, DataType offendingType,
                                                   DataTypeSet supportedTypes, const ErrorContext& context)
    : ProcessorError(ErrorCode::UnsupportedDataType, message, context)
    , offendingType_(offendingType)
    , supportedTypes_(supportedTypes) {}

DataType UnsupportedDataTypeError::offendingType() const noexcept { return offendingType_; }
DataTypeSet UnsupportedDataTypeError::supportedTypes() const noexcept { return supportedTypes_; }

ChannelArityError::ChannelArityError(const char* message, int32_t offendingChannelCount,
                                     const ErrorContext& context)
    : ProcessorError(ErrorCode::ChannelArity, message, context)
    , offendingChannelCount_(offendingChannelCount) {}

int32_t ChannelArityError::offendingChannelCount() const noexcept { return offendingChannelCount_; }

StoreFormatError::StoreFormatError(const char* message, int32_t storeVersion, int32_t libraryVersion,
                                   const ErrorContext& context)
    : ProcessorError(ErrorCode::StoreFormat, message, context)
    , storeVersion_(storeVersion)
    , libraryVersion_(libraryVersion) {}

int32_t StoreFormatError::storeVersion() const noexcept { return storeVersion_; }
int32_t StoreFormatError::libraryVersion() const noexcept { return libraryVersion_; }

StageError::StageError(const char* message, const ErrorContext& context)
    : ProcessorError(ErrorCode::Stage, message, context) {}

TileError::TileError(const char* message, const ErrorContext& context)
    : ProcessorError(ErrorCode::Tile, message, context) {}

CancelledError::CancelledError(const char* message, const ErrorContext& context)
    : ProcessorError(ErrorCode::Cancelled, message, context) {}

IoError::IoError(const char* message, const ErrorContext& context)
    : ProcessorError(ErrorCode::Io, message, context) {}

InternalError::InternalError(const char* message, const ErrorContext& context)
    : ProcessorError(ErrorCode::Internal, message, context) {}

} // namespace slideio::processor
